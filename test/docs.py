"""JSON documents over the C ABI: whole and field writes, modifications, reads, gists and typed gathers."""

import gc
import json
import struct

import numpy
import pytest
import ustore

BATCH_SIZES = [0, 1, 16, 17, 65, 500]

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
    "bool": ("?", bool),
    "f64": ("d", float),
    "f32": ("f", float),
    "i64": ("q", int),
    "i32": ("i", int),
    "i16": ("h", int),
    "i8": ("b", int),
    "u64": ("Q", int),
    "u32": ("I", int),
    "u16": ("H", int),
    "u8": ("B", int),
}


def decoded(documents: ustore.Values) -> list:
    return [None if document is None else json.loads(bytes(document)) for document in documents]


def bit(bitmap: memoryview, row: int) -> bool:
    return bool(bitmap[row // 8] >> (row % 8) & 1)


# region Whole Documents


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("spelling", DOCUMENT_SPELLINGS)
def test_whole_documents_round_trip(view, count: int, spelling: str):
    documents = [{"key": key, "name": f"name-{key}", "tags": list(range(key % 4))} for key in range(count)]
    view.docs_write(list(range(count)), [DOCUMENT_SPELLINGS[spelling](document) for document in documents])
    assert decoded(view.docs_read(list(range(count)))) == documents


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_one_document_broadcasts_to_every_key(view, count: int):
    view.docs_write(list(range(count)), '{"same": true}')
    assert decoded(view.docs_read(list(range(count)))) == [{"same": True}] * count


@pytest.mark.parametrize("keys", [[1, 2, 3], numpy.array([1, 2, 3], dtype=numpy.int64)], ids=["list", "numpy"])
def test_none_removes_documents(view, keys):
    view.docs_write(keys, "{}")
    view.docs_write(keys, None)
    assert decoded(view.docs_read(keys)) == [None, None, None]


@pytest.mark.parametrize("removed", [0, 1, 2])
def test_none_inside_a_batch_removes_one_document(view, removed: int):
    view.docs_write([0, 1, 2], "[]")
    view.docs_write([0, 1, 2], ["[1]" if key != removed else None for key in range(3)])
    assert decoded(view.docs_read([0, 1, 2])) == [None if key == removed else [1] for key in range(3)]


@pytest.mark.parametrize(
    ("id_field", "spell"), [("_id", lambda key: {"_id": key}), ("/meta/id", lambda key: {"meta": {"id": key}})]
)
@pytest.mark.parametrize("count", [1, 17])
def test_keys_are_read_out_of_an_id_field(view, id_field: str, spell, count: int):
    documents = [{**spell(key * 10), "value": key} for key in range(count)]
    view.docs_write(None, [json.dumps(document) for document in documents], id_field=id_field)
    assert decoded(view.docs_read([key * 10 for key in range(count)])) == documents


def test_documents_read_back_as_zero_copy_views(view):
    view.docs_write([1], '{"a": 1}')
    found = view.docs_read([1])
    assert isinstance(found, ustore.Values)
    held = found[0]
    del found
    gc.collect()
    assert json.loads(bytes(held)) == {"a": 1}


# endregion Whole Documents

# region Fields


@pytest.mark.parametrize(("field", "as_json", "as_string"), FIELD_READS)
def test_field_reads(view, field: str, as_json: bytes | None, as_string: bytes | None):
    view.docs_write([6], json.dumps(NESTED))
    for kind, expected in (("json", as_json), ("str", as_string)):
        found = view.docs_read([6], field=field, type=kind)[0]
        assert (None if found is None else bytes(found)) == expected


def test_a_field_of_a_missing_document_reads_as_none(view):
    assert view.docs_read([404], field="name")[0] is None


@pytest.mark.parametrize(
    ("field", "value", "expected"),
    [
        ("a", "7", {"a": 7, "b": {"c": [1, 2]}}),
        ("/b/z", '"new"', {"a": 1, "b": {"c": [1, 2], "z": "new"}}),
        ("/b/c/0", "9", {"a": 1, "b": {"c": [9, 2]}}),
        ("b", "null", {"a": 1, "b": None}),
    ],
)
@pytest.mark.parametrize("modification", ["upsert", "merge"])
def test_field_writes(view, field: str, value: str, expected: dict, modification: str):
    view.docs_write([1], json.dumps({"a": 1, "b": {"c": [1, 2]}}))
    view.docs_write([1], value, field=field, modification=modification)
    assert decoded(view.docs_read([1])) == [expected]


@pytest.mark.parametrize(("field", "expected"), [("a", {"b": {"c": [1, 2]}}), ("/b/c", {"a": 1, "b": {}})])
def test_none_removes_a_field(view, field: str, expected: dict):
    view.docs_write([1], json.dumps({"a": 1, "b": {"c": [1, 2]}}))
    view.docs_write([1], None, field=field)
    assert decoded(view.docs_read([1])) == [expected]


@pytest.mark.parametrize(
    ("type_name", "raw", "expected"),
    [
        ("json", b"[1, 2]", [1, 2]),
        ("str", b"hello", "hello"),
        ("null", b"", None),
        ("bool", b"\x01", True),
        ("f64", struct.pack("d", 1.5), 1.5),
        ("f32", struct.pack("f", 0.25), 0.25),
        ("i64", struct.pack("q", -5), -5),
        ("i32", struct.pack("i", -6), -6),
        ("i16", struct.pack("h", -7), -7),
        ("i8", struct.pack("b", -8), -8),
        ("u64", struct.pack("Q", 2**40), 2**40),
        ("u32", struct.pack("I", 2**31), 2**31),
        ("u16", struct.pack("H", 65535), 65535),
        ("u8", b"\xff", 255),
    ],
)
def test_typed_field_writes(view, type_name: str, raw: bytes, expected):
    view.docs_write([5], "{}")
    view.docs_write([5], raw, field="v", type=type_name)
    assert decoded(view.docs_read([5])) == [{"v": expected}]


# endregion Fields

# region Modifications


@pytest.mark.parametrize(
    ("modification", "modifier", "expected"),
    [
        ("upsert", {"a": 2, "d": 3}, {"a": 2, "d": 3}),
        ("update", {"a": 2, "d": 3}, {"a": 2, "d": 3}),
        ("merge", {"a": 2, "d": 3, "b": None}, {"a": 2, "d": 3}),
        ("merge", {"b": {"e": 4}}, {"a": 1, "b": {"c": [1, 2], "e": 4}}),
        (
            "patch",
            [{"op": "replace", "path": "/a", "value": 5}, {"op": "add", "path": "/e", "value": 6}],
            {"a": 5, "b": {"c": [1, 2]}, "e": 6},
        ),
        ("patch", [{"op": "remove", "path": "/b"}], {"a": 1}),
        ("patch", [{"op": "copy", "from": "/a", "path": "/f"}], {"a": 1, "b": {"c": [1, 2]}, "f": 1}),
        ("patch", [{"op": "move", "from": "/a", "path": "/g"}], {"b": {"c": [1, 2]}, "g": 1}),
    ],
)
def test_whole_document_modifications(view, modification: str, modifier, expected: dict):
    view.docs_write([1], json.dumps({"a": 1, "b": {"c": [1, 2]}}))
    view.docs_write([1], json.dumps(modifier), modification=modification)
    assert decoded(view.docs_read([1])) == [expected]


@pytest.mark.parametrize("modification", ["upsert", "insert", "merge"])
def test_whole_document_modifications_create_missing_documents(view, modification: str):
    view.docs_write([2], '{"a": 1}', modification=modification)
    assert decoded(view.docs_read([2])) == [{"a": 1}]


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.docs_write([1], "{}", modification="insert"), ustore.ExistsError),
        (lambda view: view.docs_write([404], "1", field="a"), ustore.MissingError),
        (lambda view: view.docs_write([1], "1", field="/nowhere/deep", modification="update"), ustore.MissingError),
        (lambda view: view.docs_write([1], None, field="absent"), ustore.MissingError),
        (lambda view: view.docs_write([1], "[{}]", modification="patch"), ustore.ArgumentError),
        (
            lambda view: view.docs_write([1], '[{"op": "remove", "path": "/nowhere"}]', modification="patch"),
            ustore.ArgumentError,
        ),
        (lambda view: view.docs_write([2], "{}", modification="update"), ustore.MissingError),
        (lambda view: view.docs_write([2], "[]", modification="patch"), ustore.MissingError),
        (lambda view: view.docs_write([2], "{bad"), ustore.MalformedError),
        (lambda view: view.docs_write(None, ['{"_id": "text"}'], id_field="_id"), ustore.MalformedError),
        (lambda view: view.docs_write([1], b"zz", field="v", type="bytes"), ustore.UnsupportedError),
    ],
    ids=[
        "insert-existing",
        "field-of-missing",
        "update-unresolved",
        "remove-absent",
        "bad-patch",
        "inapplicable-patch",
        "update-missing",
        "patch-missing",
        "bad-json",
        "text-id",
        "bytes-type",
    ],
)
def test_engine_refusals(view, call, failure: type):
    view.docs_write([1], '{"a": 1}')
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
def test_gist_lists_every_leaf_once(view, documents: list[dict], expected: tuple[str, ...]):
    keys = list(range(len(documents)))
    if documents:
        view.docs_write(keys, [json.dumps(document) for document in documents])
    assert view.docs_gist([*keys, -1]) == expected


# endregion Gists

# region Gathers


@pytest.mark.parametrize("type_name", SCALAR_TYPES)
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
def test_gather_scalars(view, type_name: str, value, valid: bool, converted: bool | None, collides: bool):
    """Every scalar column converts what it can and records what it could not; exactness varies by source type."""
    view.docs_write([0, 1], [json.dumps({"v": value}), "{}"])
    columns = view.docs_gather([0, 1], ["v"], type_name)
    format_char, python_type = SCALAR_TYPES[type_name]
    column = columns[0]
    assert column.format == format_char
    assert len(column) == 2
    assert bit(columns.validity(0), 0) == valid
    assert bit(columns.collision(0), 0) == collides
    assert not bit(columns.validity(0), 1)
    if converted is not None:
        assert bit(columns.conversion(0), 0) == converted
    if valid:
        assert column[0] == python_type(value)


@pytest.mark.parametrize(
    ("type_name", "value"),
    [("u8", 256), ("u8", -1), ("i8", 128), ("i8", -129), ("u64", -1), ("i64", 2**63), ("i32", 1e10), ("f32", 1e39)],
)
def test_gather_refuses_a_value_past_the_range_of_its_column(view, type_name: str, value):
    view.docs_write([0], json.dumps({"v": value}))
    columns = view.docs_gather([0], ["v"], type_name)
    assert not bit(columns.validity(0), 0)
    assert bit(columns.collision(0), 0)
    assert columns[0][0] == 0


@pytest.mark.parametrize("type_name", ["str", "bytes"])
@pytest.mark.parametrize(
    ("value", "text"), [("x", "x"), (3, "3"), (2.5, "2.5"), (True, "true"), ("", ""), (None, None), ({"k": 1}, None)]
)
def test_gather_strings(view, type_name: str, value, text: str | None):
    view.docs_write([0], json.dumps({"v": value}))
    column = view.docs_gather([0], ["v"], type_name)[0]
    expected = None if text is None else text if type_name == "str" else text.encode()
    assert column == (expected,)


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_gather_columns_line_up_with_keys(view, count: int):
    keys = list(range(count))
    view.docs_write(keys, [json.dumps({"n": key, "name": f"row-{key}", "half": key / 2}) for key in keys])
    columns = view.docs_gather([*keys, -1], ["n", "name", "half"], ["i64", "str", "f64"])
    assert len(columns) == 3
    assert columns.rows == count + 1
    assert columns.types == ("i64", "str", "f64")
    assert columns[0].tolist()[:count] == keys
    assert columns[1] == (*(f"row-{key}" for key in keys), None)
    assert numpy.asarray(columns[2])[:count].tolist() == [key / 2 for key in keys]
    assert not bit(columns.validity(0), count)


def test_gathered_views_outlive_their_columns(view):
    view.docs_write([0, 1], ['{"n": 1}', '{"n": 2}'])
    column = view.docs_gather([0, 1], ["n"], "i64")[0]
    gc.collect()
    assert column.readonly
    assert column.tolist() == [1, 2]


def test_gather_of_no_fields(view):
    columns = view.docs_gather([0], [], [])
    assert len(columns) == 0
    assert columns.rows == 1


@pytest.mark.parametrize("index", [1, -2])
def test_gather_rejects_an_index_past_the_fields(view, index: int):
    view.docs_write([0], "{}")
    columns = view.docs_gather([0], ["a"], "i64")
    with pytest.raises(IndexError):
        columns[index]
    with pytest.raises(IndexError):
        columns.validity(index)


# endregion Gathers

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.docs_write([1, 2], ["{}"]), ValueError),
        (lambda view: view.docs_write(None, ["{}"]), TypeError),
        (lambda view: view.docs_write(None, "{}", id_field="_id"), TypeError),
        (lambda view: view.docs_write([1], 42), TypeError),
        (lambda view: view.docs_write([1], "{}", modification="replace"), ValueError),
        (lambda view: view.docs_write([1], "{}", type="f16"), ValueError),
        (lambda view: view.docs_write([1], "{}", field=3), TypeError),
        (lambda view: view.docs_read([1], typ="json"), TypeError),
        (lambda view: view.docs_gather([1], "a", "i64"), TypeError),
        (lambda view: view.docs_gather([1], ["a", "b"], ["i64"]), ValueError),
        (lambda view: view.docs_gather([1], ["a"], "json"), ValueError),
        (lambda view: view.docs_gather([1], ["a"], "i128"), ValueError),
        (lambda view: view.docs_gather([1], [3], "i64"), TypeError),
    ],
    ids=[
        "width",
        "keyless-without-id-field",
        "keyless-broadcast",
        "integer-document",
        "modification",
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
def test_binding_refusals(view, call, failure: type):
    with pytest.raises(failure):
        call(view)


# endregion Refusals
