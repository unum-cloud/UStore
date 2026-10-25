"""The Pandas table view over document collections, checked against frames Pandas builds from the same rows."""

import importlib.util
import json
from uuid import UUID

import numpy as np
import pandas
import pytest
from capabilities import key, keys
from pandas.testing import assert_frame_equal, assert_series_equal

from ustore import Modality
from ustore.table_view import Table

PEOPLE = [
    {"name": "Lex", "lastname": "Fridman", "tweets": 2221},
    {"name": "Andrew", "lastname": "Huberman", "tweets": 3935},
    {"name": "Joe", "lastname": "Rogan", "tweets": 45900},
]

INTEGER_DTYPES = ["int64", "int32", "int16", "uint64", "uint32", "uint16"]
NUMBER_DTYPES = [*INTEGER_DTYPES, "float64", "float32"]


def frame(rows: dict[str, list], integers: list[int], dtypes: dict[str, str] | None = None) -> pandas.DataFrame:
    """The frame Pandas builds from the same columns, typed as the view gathers them."""
    index = pandas.Index(keys(integers), dtype=object)
    typed = dtypes or {}
    return pandas.DataFrame(
        {name: pandas.Series(values, index=index, dtype=typed.get(name)) for name, values in rows.items()}, index=index
    )


@pytest.fixture
def people(view, docs) -> Table:
    return Table.from_records(view, PEOPLE, collection=docs)


# region Exports


@pytest.mark.parametrize("dtype", [dtype for dtype in NUMBER_DTYPES if dtype != "int16"])
def test_one_typed_column(people, dtype: str):
    exported = people[["tweets"]].astype(dtype).to_pandas()
    assert_frame_equal(exported, frame({"tweets": [row["tweets"] for row in PEOPLE]}, [0, 1, 2], {"tweets": dtype}))


@pytest.mark.parametrize(
    "selection",
    [["name"], ["name", "tweets"], ["lastname", "name"], ["name", "tweets", "lastname"]],
    ids=lambda selection: "-".join(selection),
)
@pytest.mark.parametrize("typed", ["typed", "decoded"])
def test_selected_columns(people, selection: list[str], typed: str):
    dtypes = {"name": "str", "lastname": "str", "tweets": "int64"}
    view = (
        people[selection].astype({column: dtypes[column] for column in selection})
        if typed == "typed"
        else people[selection]
    )
    expected = frame(
        {column: [row[column] for row in PEOPLE] for column in selection},
        [0, 1, 2],
        dtypes if typed == "typed" else None,
    )
    assert_frame_equal(view.to_pandas(), expected, check_dtype=typed == "typed")


def test_every_column_by_default(people):
    assert people.columns == ["name", "lastname", "tweets"]
    assert people.index == keys([0, 1, 2])
    assert people.to_pandas().to_dict(orient="records") == PEOPLE


@pytest.mark.parametrize("dtype", ["str", "bytes"])
def test_text_columns(people, dtype: str):
    column = people[["name"]].astype(dtype).to_pandas()["name"]
    names = [row["name"] for row in PEOPLE]
    assert list(column) == (names if dtype == "str" else [name.encode() for name in names])


@pytest.mark.parametrize("dtype", [*INTEGER_DTYPES, "float64", "bool"])
def test_missing_values_mask_a_typed_column(view, docs, dtype: str):
    Table.from_records(view, [{"v": 1}, {"other": 2}, {"v": 0}], collection=docs)
    column = Table(view, docs).astype({"v": dtype}).to_pandas()["v"]
    assert column.isna().tolist() == [False, True, False]
    assert column.iloc[0] == 1 and column.iloc[2] == 0


def test_nested_fields_are_json_pointers(view, docs):
    Table.from_records(view, [{"user": {"name": "Lex", "id": 7}}, {"user": {"name": "Joe", "id": 8}}], collection=docs)
    table = Table(view, docs)
    assert table.columns == ["/user/name", "/user/id"]
    exported = table.astype({"/user/id": "int64"}).to_pandas()
    assert exported["/user/name"].tolist() == ["Lex", "Joe"]
    assert exported["/user/id"].tolist() == [7, 8]


def test_one_column_as_a_series(people):
    assert_series_equal(
        people["tweets"],
        pandas.Series([2221, 3935, 45900], index=pandas.Index(keys([0, 1, 2]), dtype=object), name="tweets"),
    )


def test_to_csv(people, tmp_path):
    path = tmp_path / "people.csv"
    typed = people[["name", "tweets"]].astype({"name": "str", "tweets": "int64"})
    typed.to_csv(path)
    exported = pandas.read_csv(path, index_col=0, converters={0: UUID})
    assert_frame_equal(exported, typed.to_pandas(), check_index_type=False, check_names=False)


@pytest.mark.skipif(importlib.util.find_spec("pyarrow") is None, reason="Parquet needs pyarrow")
def test_to_parquet(people, tmp_path):
    path = tmp_path / "people.parquet"
    typed = people[["name", "tweets"]].astype({"name": "str", "tweets": "int32"})
    # Arrow does not round-trip a `uuid.UUID` index across its releases, so the keys stay behind.
    typed.to_parquet(path, index=False)
    assert_frame_equal(pandas.read_parquet(path), typed.to_pandas().reset_index(drop=True))


@pytest.mark.parametrize("destination", ["string", "file"])
def test_to_json(people, tmp_path, destination: str):
    typed = people[["name", "tweets"]].astype({"name": "str", "tweets": "int32"})
    labels = [str(key(row)) for row in range(3)]
    expected = {
        "name": dict(zip(labels, ["Lex", "Andrew", "Joe"], strict=True)),
        "tweets": dict(zip(labels, [2221, 3935, 45900], strict=True)),
    }
    if destination == "string":
        assert json.loads(typed.to_json()) == expected
    else:
        typed.to_json(tmp_path / "people.json")
        assert json.loads((tmp_path / "people.json").read_text()) == expected


# endregion Exports

# region Shape and Selection


@pytest.mark.parametrize(
    ("records", "shape"),
    [
        (
            [
                {"name": "Lex", "tweets": 0},
                {"name": "Joe", "lastname": "Rogan"},
                {"name": "Andrew", "lastname": "Huberman", "tweets": 1},
            ],
            (3, 3),
        ),
        (
            [{"name": "Lex", "lastname": "Fridman", "tweets": 0}, {"name": "Joe", "lastname": "Rogan", "tweets": 2}],
            (2, 3),
        ),
        ([{"only": 1}], (1, 1)),
        ([], (0, 0)),
    ],
    ids=["ragged", "rectangular", "single", "none"],
)
def test_shape_size_and_emptiness(view, docs, records: list[dict], shape: tuple[int, int]):
    table = Table.from_records(view, records, collection=docs)
    assert table.shape == shape
    assert table.size == shape[0] * shape[1]
    assert table.empty == (0 in shape)
    assert len(table) == shape[0]


@pytest.mark.parametrize("count", [0, 1, 2, 3, 5])
def test_head(people, count: int):
    assert people.head(count).index == keys([0, 1, 2][:count])


@pytest.mark.parametrize("count", [1, 3, 7])
def test_sample(view, docs, count: int):
    records = [
        {"name": name, "tweets": index}
        for index, name in enumerate(["Lex", "Andrew", "Joe", "Charls", "Carl", "Jack", "Fred"])
    ]
    table = Table.from_records(view, records, collection=docs).astype({"name": "str", "tweets": "int64"})
    for _ in range(5):
        drawn = table.sample(count).to_pandas()
        assert len(drawn) <= count
        assert set(drawn["name"]) <= {record["name"] for record in records}
        assert all(records[row.int]["tweets"] == tweets for row, tweets in drawn["tweets"].items())


def test_rejects_an_unknown_dtype(people):
    with pytest.raises(ValueError):
        people.astype({"tweets": "int128"})


# endregion Shape and Selection

# region Writes


@pytest.mark.parametrize(
    "values",
    [[b"Fridman", b"Rogan"], ["Fridman", "Rogan"], np.array(["Fridman", "Rogan"])],
    ids=["bytes", "str", "numpy"],
)
def test_assigning_a_column(view, docs, values):
    table = Table.from_records(view, [{"name": "Lex"}, {"name": "Joe"}], collection=docs)
    table["lastname"] = values
    assert table.astype("str").to_pandas().to_dict(orient="records") == [
        {"name": "Lex", "lastname": "Fridman"},
        {"name": "Joe", "lastname": "Rogan"},
    ]


@pytest.mark.parametrize("value", [0, 2.5, "same", True, None])
def test_assigning_one_value_to_every_row(people, value):
    people["flag"] = value
    column = people["flag"]
    if value is None:
        assert column.isna().all()
    else:
        assert column.tolist() == [value] * 3


def test_assigning_a_column_of_the_wrong_length(people):
    with pytest.raises(ValueError):
        people["flag"] = [1, 2]


def test_update(people):
    people.update(pandas.DataFrame({"tweets": [2, 4, 5], "name": ["Jack", "Charls", "Sam"]}))
    assert people.to_pandas().to_dict(orient="records") == [
        {"name": "Jack", "lastname": "Fridman", "tweets": 2},
        {"name": "Charls", "lastname": "Huberman", "tweets": 4},
        {"name": "Sam", "lastname": "Rogan", "tweets": 5},
    ]


def test_update_skips_missing_cells_and_foreign_rows(people):
    people.update(pandas.DataFrame({"tweets": [7.0, None]}, index=[1, 99]))
    assert people["tweets"].tolist() == [2221, 7, 45900]
    assert people.index == keys([0, 1, 2])


def test_updating_selected_rows_merges_into_their_documents(people):
    people.loc[[key(1)]].update({"tweets": 100})
    assert people.to_pandas().to_dict(orient="records") == [PEOPLE[0], {**PEOPLE[1], "tweets": 100}, PEOPLE[2]]


def test_updating_several_selected_rows_at_once(people):
    people.loc[[0, 2]].update({"tweets": 0, "verified": True})
    assert people["tweets"].tolist() == [0, 3935, 0]
    assert people["verified"].tolist() == [True, None, True]


def test_updating_a_missing_row_of_a_collection_by_id(database, docs, view):
    availability = database.collection_create("availability", modality=Modality.DOCS)
    Table(view, availability).loc[[key(7)]].update({"quantity": 100})
    assert Table(view, availability).to_pandas().to_dict(orient="records") == [{"quantity": 100}]
    assert Table(view, docs).empty


@pytest.mark.parametrize(
    "dropped", [["name"], ["tweets"], ["name", "lastname"], "name"], ids=["name", "tweets", "two", "bare"]
)
def test_drop(people, dropped):
    typed = people[["name", "lastname", "tweets"]].astype({"name": "str", "lastname": "str", "tweets": "int64"})
    typed.drop(dropped)
    names = [dropped] if isinstance(dropped, str) else dropped
    exported = typed.to_pandas()
    for column in ["name", "lastname", "tweets"]:
        assert exported[column].isna().all() == (column in names)
    assert Table(people.view, people.collection).columns == [
        column for column in ["name", "lastname", "tweets"] if column not in names
    ]


def test_drop_skips_rows_without_the_column(view, docs):
    table = Table.from_records(view, [{"name": "Lex", "tweets": 0}, {"name": "Joe"}], collection=docs)
    table.drop("tweets")
    assert table.columns == ["name"]


def test_rename(view, docs):
    data = [{"col1": 3, "col2": "a"}, {"col1": 2, "col2": "b"}, {"col1": 1}]
    table = Table.from_records(view, data, collection=docs)
    table.rename({"col1": "column_1", "col2": "column_2"})
    assert table.columns == ["column_1", "column_2"]
    assert table.astype({"column_1": "int64"}).to_pandas()["column_1"].tolist() == [3, 2, 1]
    assert table.to_pandas()["column_2"].tolist()[:2] == ["a", "b"]


def test_merge_from(database):
    first = Table.from_records(
        database,
        [{"name": "Lex", "tweets": 0}, {"name": "Andrew", "tweets": 1}, {"name": "Joe", "tweets": 2}],
        collection=database.collection_create("first", modality=Modality.DOCS),
    )
    second = Table.from_records(
        database,
        [
            {"name": "Lex", "lastname": "Fridman", "tweets": 10},
            {"name": "Charls", "lastname": "Huberman"},
            {"name": "Carl", "lastname": "Rogan", "tweets": 3},
        ],
        collection=database.collection_create("second", modality=Modality.DOCS),
        index=[0, 1, 3],
    )
    first.merge_from(second)
    assert first.astype({"name": "str", "lastname": "str", "tweets": "int64"}).to_pandas().to_dict(
        orient="records"
    ) == [
        {"name": "Lex", "tweets": 10, "lastname": "Fridman"},
        {"name": "Charls", "tweets": 1, "lastname": "Huberman"},
        {"name": "Joe", "tweets": 2, "lastname": np.nan},
        {"name": "Carl", "tweets": 3, "lastname": "Rogan"},
    ]


# endregion Writes

# region Construction


@pytest.mark.parametrize("index", [None, [10, 20, 30, 40]], ids=["positions", "keys"])
def test_from_dict(view, docs, index: list[int] | None):
    data = {"col1": [3, 2, 1, 0], "col2": ["a", "b", "c", "d"]}
    table = Table.from_dict(view, data, index=index, collection=docs)
    assert table.index == keys(index or [0, 1, 2, 3])
    assert_frame_equal(
        table.to_pandas(), pandas.DataFrame(data, index=pandas.Index(keys(index or [0, 1, 2, 3]), dtype=object))
    )


@pytest.mark.parametrize("dtype", ["int64", "uint8", "float32"])
def test_from_records_with_numpy_scalars(view, docs, dtype: str):
    values = np.array([3, 2, 1], dtype=dtype)
    table = Table.from_records(view, [{"v": value} for value in values], collection=docs)
    assert table.astype({"v": dtype}).to_pandas()["v"].tolist() == values.tolist()


def test_from_dict_rejects_ragged_columns(view, docs):
    with pytest.raises(ValueError):
        Table.from_dict(view, {"col1": [1, 2], "col2": [1]}, collection=docs)


# endregion Construction
