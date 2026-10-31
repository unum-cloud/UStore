"""Pages over the C ABI: imports, edits, renders, scans and cursors on every engine that serves them.

One random edit stream runs on a database of every engine at once, and every engine must render every page as the
others do after every call.
"""

import random
from pathlib import Path
from uuid import UUID

import pytest
from capabilities import ENGINES, key, open_engine

import ustore
from ustore import Modality, PageEncoding

PAGE = key(500)
NIL = key(0)


def serves_pages(database: ustore.Database) -> UUID | None:
    """The pages collection of `database`, or None when its engine serves no pages, as NVMe does not yet."""
    try:
        collection = database.collection_create("pages", modality=Modality.PAGES)
        database.pages_import(key(1), b"<probe/>", collection=collection)
    except ustore.UnsupportedError:
        return None
    return collection


@pytest.fixture
def pages(database: ustore.Database) -> UUID:
    collection = serves_pages(database)
    if collection is None:
        pytest.skip("the engine serves no pages")
    return collection


def render(database, pages: UUID, element: UUID | None = None, encoding: PageEncoding = PageEncoding.XML):
    """The rendering of `element` of `PAGE`, or of the whole page, and None where it is missing."""
    rendered = database.pages_read(PAGE, ustore.Arena(), elements=element, outputs_encoding=encoding, collection=pages)
    return None if rendered[0] is None else bytes(rendered[0])


# region Cases


def test_edit_and_render(database, pages):
    """Every verb lands where its header says, and renders back as XML and text, whole and by subtree."""
    database.pages_import(PAGE, '<doc><p id="1">one</p></doc>', collection=pages)
    assert database.pages_insert(PAGE, b"<h1>title</h1>", elements=key(10), collection=pages) == (key(10),)
    assert render(database, pages) == b'<h1>title</h1><doc><p id="1">one</p></doc>'
    (minted,) = database.pages_insert(PAGE, "<i>x</i>", parents=key(10), anchors=NIL, collection=pages)
    assert minted != NIL
    assert render(database, pages, key(10)) == b"<h1>title<i>x</i></h1>"
    database.pages_splice(PAGE, key(10), 0, 5, "Head", collection=pages)
    database.pages_set_attributes(PAGE, key(10), "class", "a<b", collection=pages)
    assert render(database, pages, key(10)) == b'<h1 class="a&lt;b">Head<i>x</i></h1>'
    database.pages_move(PAGE, minted, collection=pages)
    database.pages_remove_attributes(PAGE, key(10), "class", collection=pages)
    assert render(database, pages) == b'<i>x</i><h1>Head</h1><doc><p id="1">one</p></doc>'
    assert render(database, pages, encoding=PageEncoding.TEXT) == b"xHeadone"
    database.pages_remove(PAGE, key(10), collection=pages)
    assert render(database, pages, key(10)) is None
    database.pages_remove(PAGE, NIL, collection=pages)
    assert render(database, pages) is None


def test_refusals(database, pages):
    """HTML, Markdown and JSON are unsupported, broken XML is malformed, and an edit of a missing page is not found."""
    for encoding in (PageEncoding.HTML, PageEncoding.MARKDOWN, PageEncoding.JSON):
        with pytest.raises(ustore.UnsupportedError):
            database.pages_import(PAGE, b"<a/>", encoding=encoding, collection=pages)
        with pytest.raises(ustore.UnsupportedError):
            database.pages_read(key(1), ustore.Arena(), outputs_encoding=encoding, collection=pages)
    for markup in (b"<a>", b"<a>&nbsp;</a>", b"a & b", b'<a b="<"/>'):
        with pytest.raises(ustore.MalformedError):
            database.pages_import(PAGE, markup, collection=pages)
    with pytest.raises(ustore.NotFoundError):
        database.pages_splice(key(404), key(404), 0, 0, b"x", collection=pages)
    database.pages_import(PAGE, b"a & b", encoding=PageEncoding.TEXT, collection=pages)
    assert render(database, pages) == b"a &amp; b"


def test_one_item_stands_for_every_task(database, pages):
    """One page and one parent serve a batch of fragments, while batches of two lengths are refused."""
    database.pages_import(PAGE, b"<doc/>", collection=pages)
    landed = database.pages_insert(PAGE, [b"<a/>", b"<b/>"], elements=[key(20), key(21)], collection=pages)
    assert landed == (key(20), key(21))
    assert render(database, pages) == b"<b/><a/><doc/>"
    with pytest.raises(ustore.InvalidArgumentError):
        database.pages_splice(PAGE, [key(20), key(21)], [0, 0, 0], 0, b"", collection=pages)


def test_scan_and_cursor_keep_what_the_filter_matches(database, pages):
    """A scan and a cursor list the pages holding a node of their XPath filter, and the cursor renders them."""
    database.pages_import([key(2), key(3)], [b"<p><b/></p>", b"<p/>"], collection=pages)
    arena = ustore.Arena()
    assert database.pages_scan(key(2), 10, arena, filter="//b", collection=pages)[0] == (key(2),)
    with database.pages_cursor(key(2), filter="//b", collection=pages) as cursor:
        page = cursor.next(10, arena, values=True)
        assert list(page) == [key(2)]
        assert bytes(page.values[0]) == b"<p><b/></p>"


def test_a_deferred_import_lands_once_waited(database, pages, later):
    later.pages_import(PAGE, b"<later/>", collection=pages).wait()
    assert bytes(later.pages_read(PAGE, ustore.Arena(), collection=pages).wait()[0]) == b"<later/>"


# The arguments every verb takes by position after the page, in order; the rest of a task's columns are keywords.
POSITIONAL = {
    "import": ["markup"],
    "insert": ["fragments"],
    "remove": ["elements"],
    "move": ["elements"],
    "splice": ["elements", "starts", "lengths", "texts"],
    "set_attributes": ["elements", "fields", "values"],
    "remove_attributes": ["elements", "fields"],
}


class Stream:
    """A random stream of page calls, aimed mostly at the elements it gave keys to."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.given: list[UUID] = []
        self.imported = False

    def text(self, longest: int) -> bytes:
        return bytes(self.rng.choice(b"abcxyz &<>\"'") for _ in range(self.rng.randint(0, longest)))

    def any_key(self) -> UUID:
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

    def call(self) -> tuple[str, list[dict]]:
        """The verb of the next call and its tasks, each a column name onto its item."""
        roll = self.rng.randrange(100)
        if roll < 4 or not self.imported:
            self.imported = True
            body = b"".join(self.fragment() for _ in range(self.rng.randrange(4)))
            return "import", [{"markup": b"<doc>" + body + b"</doc>"}]
        tasks = []
        for _ in range(self.rng.randint(1, 3)):
            if roll < 40:
                self.given.append(key(10_000 + len(self.given)))
                tasks.append(
                    {
                        "elements": self.given[-1],
                        "parents": self.rng.choice([NIL, self.any_key()]),
                        "anchors": self.rng.choice([NIL, self.any_key()]),
                        "fragments": self.fragment(),
                    }
                )
            elif roll < 50:
                tasks.append({"elements": self.any_key()})
            elif roll < 62:
                tasks.append(
                    {
                        "elements": self.any_key(),
                        "parents": self.rng.choice([NIL, self.any_key()]),
                        "anchors": self.rng.choice([NIL, self.any_key()]),
                    }
                )
            elif roll < 80:
                tasks.append(
                    {
                        "elements": self.any_key(),
                        "starts": self.rng.randrange(6),
                        "lengths": self.rng.randrange(4),
                        "texts": self.text(4),
                    }
                )
            elif roll < 93:
                tasks.append(
                    {"elements": self.any_key(), "fields": self.rng.choice([b"id", b"x"]), "values": self.text(3)}
                )
            else:
                tasks.append({"elements": self.any_key(), "fields": self.rng.choice([b"id", b"x"])})
        verb = (
            "insert"
            if roll < 40
            else "remove"
            if roll < 50
            else "move"
            if roll < 62
            else "splice"
            if roll < 80
            else "set_attributes"
            if roll < 93
            else "remove_attributes"
        )
        return verb, tasks


def test_random_streams_agree(tmp_path: Path, seed: int):
    """One random stream applied to every engine at once renders identically on all of them after every call."""
    engines = {}
    for engine in ENGINES:
        directory = tmp_path / engine
        directory.mkdir()
        database = open_engine(engine, directory)
        collection = serves_pages(database)
        if collection is None:
            database.close()
            continue
        engines[engine] = (database, collection)
    try:
        if len(engines) < 2:
            pytest.skip("fewer than two engines serve pages")
        stream = Stream(random.Random(seed))
        for _ in range(300):
            verb, tasks = stream.call()
            columns = {name: [task[name] for task in tasks] for name in tasks[0]}
            arguments = [columns.pop(name) for name in POSITIONAL[verb]]
            for database, collection in engines.values():
                getattr(database, f"pages_{verb}")(PAGE, *arguments, **columns, collection=collection)
            renders = {engine: render(*opened) for engine, opened in engines.items()}
            assert len(set(renders.values())) == 1, renders
            probed = stream.any_key()
            subtrees = {engine: render(*opened, probed, PageEncoding.TEXT) for engine, opened in engines.items()}
            assert len(set(subtrees.values())) == 1, subtrees
    finally:
        for database, _ in engines.values():
            database.close()


# endregion
