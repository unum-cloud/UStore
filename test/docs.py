"""JSON documents over the C ABI: whole and field writes, modifications, reads, gists and typed gathers."""

import asyncio
import gc
import json
import struct

import numpy as np
import pytest
from capabilities import BATCH_SIZES, key

import ustore
from ustore import FieldType, Modification

DOCUMENT_SPELLINGS = {
    "str": lambda document: json.dumps(document),
    "bytes": lambda document: json.dumps(document).encode(),
    "bytearray": lambda document: bytearray(json.dumps(document).encode()),
    "memoryview": lambda document: memoryview(json.dumps(document).encode()),
}

NESTED = {"s": "x", "n": 3, "f": 2.5, "t": True, "z": None, "o": {"k": 1}, "arr": [1, "two"]}

# A field, what reading it as JSON answers, and what reading it as a string answers.
FIELD_READS = [
    ("s", b'"x"', b"x"),
    ("n", b"3", b"3"),
    ("f", b"2.5", b"2.5"),
    ("t", b"true", b"true"),
    ("o", b'{"k":1}', b'{"k":1}'),
    ("/o/k", b"1", b"1"),
    ("/arr/1", b'"two"', b"two"),
    ("missing", None, None),
    ("/o/missing", None, None),
]

SCALAR_TYPES = {
    FieldType.BOOL: ("?", bool),
    FieldType.F64: ("d", float),
    FieldType.F32: ("f", float),
    FieldType.I64: ("q", int),
    FieldType.I32: ("i", int),
    FieldType.I16: ("h", int),
    FieldType.I8: ("b", int),
    FieldType.U64: ("Q", int),
    FieldType.U32: ("I", int),
    FieldType.U16: ("H", int),
    FieldType.U8: ("B", int),
}


def decoded(documents: ustore.Values) -> list:
    return [None if document is None else json.loads(bytes(document)) for document in documents]


def bit(bitmap: memoryview, row: int) -> bool:
    return bool(bitmap[row // 8] >> (row % 8) & 1)


# region Whole Documents


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("spelling", DOCUMENT_SPELLINGS)
def test_whole_documents_round_trip(view, docs, arena, count: int, spelling: str):
    documents = [{"key": key, "name": f"name-{key}", "tags": list(range(key % 4))} for key in range(count)]
    view.docs_write(
        list(range(count)), [DOCUMENT_SPELLINGS[spelling](document) for document in documents], collection=docs
    )
    assert decoded(view.docs_read(list(range(count)), arena, collection=docs)) == documents


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_one_document_broadcasts_to_every_key(view, docs, arena, count: int):
    view.docs_write(list(range(count)), '{"same": true}', collection=docs)
    assert decoded(view.docs_read(list(range(count)), arena, collection=docs)) == [{"same": True}] * count


@pytest.mark.parametrize("keys", [[1, 2, 3], np.array([1, 2, 3], dtype=np.int64)], ids=["list", "numpy"])
def test_remove_removes_documents(view, docs, arena, keys):
    view.docs_write(keys, "{}", collection=docs)
    view.docs_remove(keys, collection=docs)
    assert decoded(view.docs_read(keys, arena, collection=docs)) == [None, None, None]


@pytest.mark.parametrize("refused", [0, 1, 2, None])
def test_none_in_a_batch_is_refused(view, docs, arena, refused: int | None):
    """A None, in a batch or in place of one, is refused before anything is written."""
    view.docs_write([0, 1, 2], "[]", collection=docs)
    documents = None if refused is None else ["[1]" if key != refused else None for key in range(3)]
    with pytest.raises(TypeError):
        view.docs_write([0, 1, 2], documents, collection=docs)
    assert decoded(view.docs_read([0, 1, 2], arena, collection=docs)) == [[], [], []]


@pytest.mark.parametrize(
    ("id_field", "spell"),
    [
        ("_id", lambda integer: {"_id": integer}),
        ("/meta/id", lambda integer: {"meta": {"id": integer}}),
        ("_id", lambda integer: {"_id": str(key(integer))}),
    ],
    ids=["integer", "nested-integer", "uuid-text"],
)
@pytest.mark.parametrize("count", [1, 17])
def test_keys_are_read_out_of_an_id_field(view, docs, arena, id_field: str, spell, count: int):
    documents = [{**spell(integer * 10), "value": integer} for integer in range(count)]
    view.docs_write(None, [json.dumps(document) for document in documents], id_field=id_field, collection=docs)
    assert decoded(view.docs_read([integer * 10 for integer in range(count)], arena, collection=docs)) == documents


def test_documents_read_back_as_zero_copy_views(view, docs, arena):
    view.docs_write([1], '{"a": 1}', collection=docs)
    found = view.docs_read([1], arena, collection=docs)
    assert isinstance(found, ustore.Values)
    held = found[0]
    del found
    gc.collect()
    assert json.loads(bytes(held)) == {"a": 1}


# endregion Whole Documents

# region Fields


@pytest.mark.parametrize(("field", "as_json", "as_string"), FIELD_READS)
def test_field_reads(view, docs, arena, field: str, as_json: bytes | None, as_string: bytes | None):
    view.docs_write([6], json.dumps(NESTED), collection=docs)
    for kind, expected in ((FieldType.JSON, as_json), (FieldType.STR, as_string)):
        found = view.docs_read([6], arena, field=field, outputs_type=kind, collection=docs)
        assert (None if found[0] is None else bytes(found[0])) == expected


def test_a_field_of_a_missing_document_reads_as_none(view, docs, arena):
    assert view.docs_read([404], arena, field="name", collection=docs)[0] is None


@pytest.mark.parametrize(
    ("field", "value", "expected"),
    [
        ("a", "7", {"a": 7, "b": {"c": [1, 2]}}),
        ("/b/z", '"new"', {"a": 1, "b": {"c": [1, 2], "z": "new"}}),
        ("/b/c/0", "9", {"a": 1, "b": {"c": [9, 2]}}),
        ("b", "null", {"a": 1, "b": None}),
    ],
)
@pytest.mark.parametrize("modification", [Modification.UPSERT, Modification.MERGE])
def test_field_writes(view, docs, arena, field: str, value: str, expected: dict, modification: Modification):
    view.docs_write([1], json.dumps({"a": 1, "b": {"c": [1, 2]}}), collection=docs)
    view.docs_write([1], value, field=field, modification=modification, collection=docs)
    assert decoded(view.docs_read([1], arena, collection=docs)) == [expected]


@pytest.mark.parametrize(
    ("patch", "expected"), [({"a": None}, {"b": {"c": [1, 2]}}), ({"b": {"c": None}}, {"a": 1, "b": {}})]
)
def test_merging_null_removes_a_field(view, docs, arena, patch: dict, expected: dict):
    view.docs_write([1], json.dumps({"a": 1, "b": {"c": [1, 2]}}), collection=docs)
    view.docs_write([1], json.dumps(patch), modification=Modification.MERGE, collection=docs)
    assert decoded(view.docs_read([1], arena, collection=docs)) == [expected]


@pytest.mark.parametrize(
    ("field_type", "raw", "expected"),
    [
        (FieldType.JSON, b"[1, 2]", [1, 2]),
        (FieldType.STR, b"hello", "hello"),
        (FieldType.NULL, b"", None),
        (FieldType.BOOL, b"\x01", True),
        (FieldType.F64, struct.pack("d", 1.5), 1.5),
        (FieldType.F32, struct.pack("f", 0.25), 0.25),
        (FieldType.I64, struct.pack("q", -5), -5),
        (FieldType.I32, struct.pack("i", -6), -6),
        (FieldType.I16, struct.pack("h", -7), -7),
        (FieldType.I8, struct.pack("b", -8), -8),
        (FieldType.U64, struct.pack("Q", 2**40), 2**40),
        (FieldType.U32, struct.pack("I", 2**31), 2**31),
        (FieldType.U16, struct.pack("H", 65535), 65535),
        (FieldType.U8, b"\xff", 255),
    ],
)
def test_typed_field_writes(view, docs, arena, field_type: FieldType, raw: bytes, expected):
    view.docs_write([5], "{}", collection=docs)
    view.docs_write([5], raw, field="v", type=field_type, collection=docs)
    assert decoded(view.docs_read([5], arena, collection=docs)) == [{"v": expected}]


# endregion Fields

# region Modifications


@pytest.mark.parametrize(
    ("modification", "modifier", "expected"),
    [
        (Modification.UPSERT, {"a": 2, "d": 3}, {"a": 2, "d": 3}),
        (Modification.UPDATE, {"a": 2, "d": 3}, {"a": 2, "d": 3}),
        (Modification.MERGE, {"a": 2, "d": 3, "b": None}, {"a": 2, "d": 3}),
        (Modification.MERGE, {"b": {"e": 4}}, {"a": 1, "b": {"c": [1, 2], "e": 4}}),
        (
            Modification.PATCH,
            [{"op": "replace", "path": "/a", "value": 5}, {"op": "add", "path": "/e", "value": 6}],
            {"a": 5, "b": {"c": [1, 2]}, "e": 6},
        ),
        (Modification.PATCH, [{"op": "remove", "path": "/b"}], {"a": 1}),
        (Modification.PATCH, [{"op": "copy", "from": "/a", "path": "/f"}], {"a": 1, "b": {"c": [1, 2]}, "f": 1}),
        (Modification.PATCH, [{"op": "move", "from": "/a", "path": "/g"}], {"b": {"c": [1, 2]}, "g": 1}),
    ],
)
def test_whole_document_modifications(view, docs, arena, modification: Modification, modifier, expected: dict):
    view.docs_write([1], json.dumps({"a": 1, "b": {"c": [1, 2]}}), collection=docs)
    view.docs_write([1], json.dumps(modifier), modification=modification, collection=docs)
    assert decoded(view.docs_read([1], arena, collection=docs)) == [expected]


@pytest.mark.parametrize("modification", [Modification.UPSERT, Modification.INSERT, Modification.MERGE])
def test_whole_document_modifications_create_missing_documents(view, docs, arena, modification: Modification):
    view.docs_write([2], '{"a": 1}', modification=modification, collection=docs)
    assert decoded(view.docs_read([2], arena, collection=docs)) == [{"a": 1}]


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (
            lambda view: view.docs_write([1], "{}", modification=Modification.INSERT, collection="docs"),
            ustore.AlreadyExistsError,
        ),
        (lambda view: view.docs_write([404], "1", field="a", collection="docs"), ustore.NotFoundError),
        (
            lambda view: view.docs_write(
                [1], "1", field="/nowhere/deep", modification=Modification.UPDATE, collection="docs"
            ),
            ustore.NotFoundError,
        ),
        (
            lambda view: view.docs_write([1], "[{}]", modification=Modification.PATCH, collection="docs"),
            ustore.InvalidArgumentError,
        ),
        (
            lambda view: view.docs_write(
                [1], '[{"op": "remove", "path": "/nowhere"}]', modification=Modification.PATCH, collection="docs"
            ),
            ustore.InvalidArgumentError,
        ),
        (
            lambda view: view.docs_write([2], "{}", modification=Modification.UPDATE, collection="docs"),
            ustore.NotFoundError,
        ),
        (
            lambda view: view.docs_write([2], "[]", modification=Modification.PATCH, collection="docs"),
            ustore.NotFoundError,
        ),
        (lambda view: view.docs_write([2], "{bad", collection="docs"), ustore.MalformedError),
        (
            lambda view: view.docs_write(None, ['{"_id": "text"}'], id_field="_id", collection="docs"),
            ustore.MalformedError,
        ),
        (
            lambda view: view.docs_write([1], b"zz", field="v", type=FieldType.BIN, collection="docs"),
            ustore.UnsupportedError,
        ),
        (lambda view: view.docs_write([1], "{}", type=FieldType.F16, collection="docs"), ustore.UnsupportedError),
    ],
    ids=[
        "insert-existing",
        "field-of-missing",
        "update-unresolved",
        "bad-patch",
        "inapplicable-patch",
        "update-missing",
        "patch-missing",
        "bad-json",
        "text-id",
        "bin-type",
        "f16-type",
    ],
)
def test_engine_refusals(view, docs, call, failure: type):
    view.docs_write([1], '{"a": 1}', collection=docs)
    with pytest.raises(failure):
        call(view)


# endregion Modifications

# region Gists


@pytest.mark.parametrize(
    ("documents", "expected"),
    [
        ([{"a": 1}], ("/a",)),
        ([{"a": 1}, {"b": 2}, {"a": 3}], ("/a", "/b")),
        ([{"o": {"k": 1}, "arr": [1, 2]}], ("/o/k", "/arr/0", "/arr/1")),
        ([{}], ()),
        ([{"a/b": 1, "m~n": {"~/": 2}}], ("/a~1b", "/m~0n/~0~1")),
        ([], ()),
    ],
)
def test_gist_lists_every_leaf_once(view, docs, arena, documents: list[dict], expected: tuple[str, ...]):
    keys = list(range(len(documents)))
    if documents:
        view.docs_write(keys, [json.dumps(document) for document in documents], collection=docs)
    assert view.docs_gist([*keys, len(keys)], arena, collection=docs) == expected


# endregion Gists

# region Gathers


@pytest.mark.parametrize("field_type", SCALAR_TYPES)
@pytest.mark.parametrize(
    ("value", "valid", "converted", "collides"),
    [
        (3, True, None, False),
        (2.5, True, None, False),
        ("text", False, False, True),
        ({"k": 1}, False, False, True),
        (None, False, False, False),
    ],
    ids=["integer", "real", "text", "object", "null"],
)
def test_gather_scalars(
    view, docs, arena, field_type: FieldType, value, valid: bool, converted: bool | None, collides: bool
):
    """Every scalar column converts what it can and records what it could not; exactness varies by source type."""
    view.docs_write([0, 1], [json.dumps({"v": value}), "{}"], collection=docs)
    columns = view.docs_gather([0, 1], ["v"], field_type, arena, collection=docs)
    format_char, python_type = SCALAR_TYPES[field_type]
    column = columns[0]
    assert column.format == format_char
    assert len(column) == 2
    assert bit(columns.validities(0), 0) == valid
    assert bit(columns.collisions(0), 0) == collides
    assert not bit(columns.validities(0), 1)
    if converted is not None:
        assert bit(columns.conversions(0), 0) == converted
    if valid:
        assert column[0] == python_type(value)


@pytest.mark.parametrize(
    ("field_type", "value"),
    [
        (FieldType.U8, 256),
        (FieldType.U8, -1),
        (FieldType.I8, 128),
        (FieldType.I8, -129),
        (FieldType.U64, -1),
        (FieldType.I64, 2**63),
        (FieldType.I32, 1e10),
        (FieldType.F32, 1e39),
    ],
)
def test_gather_refuses_a_value_past_the_range_of_its_column(view, docs, arena, field_type: FieldType, value):
    view.docs_write([0], json.dumps({"v": value}), collection=docs)
    columns = view.docs_gather([0], ["v"], field_type, arena, collection=docs)
    assert not bit(columns.validities(0), 0)
    assert bit(columns.collisions(0), 0)
    assert columns[0][0] == 0


@pytest.mark.parametrize("field_type", [FieldType.STR, FieldType.BIN])
@pytest.mark.parametrize(
    ("value", "text"), [("x", "x"), (3, "3"), (2.5, "2.5"), (True, "true"), ("", ""), (None, None), ({"k": 1}, None)]
)
def test_gather_strings(view, docs, arena, field_type: FieldType, value, text: str | None):
    view.docs_write([0], json.dumps({"v": value}), collection=docs)
    column = view.docs_gather([0], ["v"], field_type, arena, collection=docs)[0]
    expected = None if text is None else text if field_type is FieldType.STR else text.encode()
    assert column == (expected,)


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_gather_columns_line_up_with_keys(view, docs, arena, count: int):
    keys = list(range(count))
    view.docs_write(
        keys, [json.dumps({"n": key, "name": f"row-{key}", "half": key / 2}) for key in keys], collection=docs
    )
    types = (FieldType.I64, FieldType.STR, FieldType.F64)
    columns = view.docs_gather([*keys, count], ["n", "name", "half"], types, arena, collection=docs)
    assert len(columns) == 3
    assert columns.rows == count + 1
    assert columns.types == types
    assert columns[0].tolist()[:count] == keys
    assert columns[1] == (*(f"row-{key}" for key in keys), None)
    assert np.asarray(columns[2])[:count].tolist() == [key / 2 for key in keys]
    assert not bit(columns.validities(0), count)


def test_gathered_views_outlive_their_columns(view, docs, arena):
    view.docs_write([0, 1], ['{"n": 1}', '{"n": 2}'], collection=docs)
    column = view.docs_gather([0, 1], ["n"], FieldType.I64, arena, collection=docs)[0]
    gc.collect()
    assert column.readonly
    assert column.tolist() == [1, 2]


def test_gather_of_no_fields(view, docs, arena):
    columns = view.docs_gather([0], [], [], arena, collection=docs)
    assert len(columns) == 0
    assert columns.rows == 1


@pytest.mark.parametrize("index", [1, -2])
def test_gather_rejects_an_index_past_the_fields(view, docs, arena, index: int):
    view.docs_write([0], "{}", collection=docs)
    columns = view.docs_gather([0], ["a"], FieldType.I64, arena, collection=docs)
    with pytest.raises(IndexError):
        columns[index]
    with pytest.raises(IndexError):
        columns.validities(index)


# endregion Gathers

# region Deferred Calls


def test_a_deferred_docs_write_lands_once_waited(view, docs, later, arena):
    """The documents and the field name stay with the call, so the caller's own may go before it runs."""
    view.docs_write([1, 2], "{}", collection=docs)
    documents = [json.dumps(NESTED), "{}"]
    writing = later.docs_write([1, 2], documents, field="o", collection=docs)
    del documents
    assert writing.wait() is None
    assert decoded(view.docs_read([1, 2], arena, collection=docs)) == [{"o": NESTED}, {"o": {}}]


def test_a_deferred_docs_read_answers_once_waited(view, docs, later, arena):
    view.docs_write([1], json.dumps(NESTED), collection=docs)
    assert decoded(later.docs_read([1, 2], arena, collection=docs).wait()) == [NESTED, None]
    assert bytes(later.docs_read([1], arena, field="s", outputs_type=FieldType.STR, collection=docs).wait()[0]) == b"x"


def test_a_deferred_docs_gist_answers_once_waited(view, docs, later, arena):
    view.docs_write([1, 2], ['{"a": {"b": 1}}', '{"c": 2}'], collection=docs)
    assert later.docs_gist([1, 2], arena, collection=docs).wait() == ("/a/b", "/c")


def test_a_deferred_docs_gather_answers_once_waited(view, docs, later, arena):
    view.docs_write([1, 2], ['{"n": 1, "s": "x"}', '{"n": 2}'], collection=docs)
    columns = later.docs_gather([1, 2], ["n", "s"], [FieldType.I64, FieldType.STR], arena, collection=docs).wait()
    assert list(columns[0]) == [1, 2]
    assert columns[1] == ("x", None)


def test_deferred_document_subscripts_answer_once_waited(view, later):
    """`docs[key]` parses what it read once waited for, and `docs_json[key]` answers a copy of the text."""
    view["docs"].docs[1] = NESTED
    collection = later["docs"]
    assert collection.docs[1].wait() == NESTED
    assert collection.docs[[1, 2]].wait() == [NESTED, None]
    assert json.loads(collection.docs_json[1].wait()) == NESTED
    missing = collection.docs[2]
    with pytest.raises(KeyError):
        missing.wait()


def test_await_documents_under_asyncio(view, docs, later, arena):
    async def main() -> tuple:
        await later.docs_write([1], json.dumps(NESTED), collection=docs)
        return await later["docs"].docs[1], await later.docs_gist([1], arena, collection=docs)

    document, paths = asyncio.run(main())
    assert document == NESTED
    assert "/o/k" in paths


# endregion Deferred Calls

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.docs_write([1, 2], ["{}"], collection="docs"), ValueError),
        (lambda view: view.docs_write(None, ["{}"], collection="docs"), TypeError),
        (lambda view: view.docs_write(None, "{}", id_field="_id", collection="docs"), TypeError),
        (lambda view: view.docs_write([1], 42, collection="docs"), TypeError),
        (lambda view: view.docs_write([1], "{}", modification=len(Modification), collection="docs"), ValueError),
        (lambda view: view.docs_write([1], "{}", modification="merge", collection="docs"), TypeError),
        (lambda view: view.docs_write([1], "{}", type=-1, collection="docs"), ValueError),
        (lambda view: view.docs_write([1], "{}", field=3, collection="docs"), TypeError),
        (lambda view: view.docs_read([1], ustore.Arena(), typ=FieldType.JSON, collection="docs"), TypeError),
        (lambda view: view.docs_gather([1], "a", FieldType.I64, ustore.Arena(), collection="docs"), TypeError),
        (
            lambda view: view.docs_gather([1], ["a", "b"], [FieldType.I64], ustore.Arena(), collection="docs"),
            ValueError,
        ),
        (lambda view: view.docs_gather([1], ["a"], FieldType.JSON, ustore.Arena(), collection="docs"), ValueError),
        (lambda view: view.docs_gather([1], ["a"], 999, ustore.Arena(), collection="docs"), ValueError),
        (lambda view: view.docs_gather([1], [3], FieldType.I64, ustore.Arena(), collection="docs"), TypeError),
    ],
    ids=[
        "width",
        "keyless-without-id-field",
        "keyless-broadcast",
        "integer-document",
        "modification",
        "modification-text",
        "write-type",
        "field-type",
        "keyword",
        "fields-as-string",
        "types-width",
        "gather-json",
        "gather-type",
        "field-name-type",
    ],
)
def test_binding_refusals(view, docs, call, failure: type):
    with pytest.raises(failure):
        call(view)


# endregion Refusals


def test_docs_filters_narrow_scans_and_cursors(view, docs, arena):
    """A JSONPath root filter narrows a scan, and a documents cursor keeps the documents its filter matched, page by
    page, with their bodies when asked."""
    view.docs_write([1, 2, 3, 4], ['{"age": 25}', '{"age": 40}', '{"name": "x"}', '{"age": 31}'], collection=docs)
    older = view.docs_scan(0, 8, arena, collection=docs, filter="?@.age > 30")
    assert [key.int for key in older[0]] == [2, 4]
    with view.docs_cursor(collection=docs, filter="?@.age > 30") as cursor:
        page = cursor.next(1, arena, values=True)
        assert [key.int for key in page] == [2]
        assert json.loads(bytes(page.values[0])) == {"age": 40}
        assert [key.int for key in cursor.next(1, arena)] == [4]
        assert not cursor.next(1, arena)
