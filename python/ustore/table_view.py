"""A Pandas table view over a collection of JSON documents in `ustore.lib`.

Rows are documents addressed by their keys, and columns are fields: top-level keys, or JSON Pointers
such as `/user/name` for nested values. A column given a dtype is gathered into typed memory and
handed to Pandas without a copy where every value is valid, while a column without one is decoded
from JSON. Verbs that change columns - assignment, `update`, `drop`, `rename` and `merge_from` -
write through to the store instead of answering a changed copy.
"""

from __future__ import annotations

import json
from collections.abc import Iterable, Mapping, Sequence
from itertools import batched
from typing import Any

import numpy
import pandas

from ustore.keys import PAGE_KEYS, scan_keys
from ustore.lib import Columns, Database, Transaction

type View = Database | Transaction

GATHER_TYPES = {
    "float64": "f64",
    "float32": "f32",
    "int64": "i64",
    "int32": "i32",
    "int16": "i16",
    "int8": "i8",
    "uint64": "u64",
    "uint32": "u32",
    "uint16": "u16",
    "uint8": "u8",
    "bool": "bool",
    "str": "str",
    "bytes": "bytes",
}

# A column of these dtypes is written as raw scalars, so nothing is printed as JSON and parsed back.
WRITE_TYPES = {
    "float64": "f64",
    "float32": "f32",
    "int64": "i64",
    "int32": "i32",
    "int16": "i16",
    "int8": "i8",
    "uint64": "u64",
    "uint32": "u32",
    "uint16": "u16",
    "uint8": "u8",
    "bool": "bool",
}

NULLABLE_DTYPES = {
    "float64": "Float64",
    "float32": "Float32",
    "int64": "Int64",
    "int32": "Int32",
    "int16": "Int16",
    "int8": "Int8",
    "uint64": "UInt64",
    "uint32": "UInt32",
    "uint16": "UInt16",
    "uint8": "UInt8",
    "bool": "boolean",
}


def encode_json(value: Any) -> str:
    """Spells a cell as JSON, reading NumPy scalars as the Python values they hold and bytes as UTF-8."""

    def fallback(unknown: Any) -> Any:
        if isinstance(unknown, bytes):
            return unknown.decode()
        if isinstance(unknown, numpy.generic):
            return unknown.item()
        raise TypeError(f"Cannot spell {type(unknown).__name__} as JSON")

    return json.dumps(value, default=fallback)


def scalar_column(values: Any) -> tuple[numpy.ndarray, str] | None:
    """One flat NumPy column of a dtype the store writes as raw scalars, or None for anything else."""
    if not isinstance(values, numpy.ndarray) or values.ndim != 1:
        return None
    kind = WRITE_TYPES.get(values.dtype.name)
    return (numpy.ascontiguousarray(values), kind) if kind else None


def column_of(pointer: str) -> str:
    """Names the column a JSON Pointer addresses: the bare key at the top level, the pointer below it."""
    segments = pointer[1:].split("/")
    return segments[0].replace("~1", "/").replace("~0", "~") if len(segments) == 1 else pointer


def value_at(document: Any, column: str) -> Any:
    """The value a column addresses inside a decoded document, or None where it holds nothing."""
    if not column.startswith("/"):
        return document.get(column) if isinstance(document, dict) else None
    for segment in column[1:].split("/"):
        key = segment.replace("~1", "/").replace("~0", "~")
        if isinstance(document, dict):
            document = document.get(key)
        elif isinstance(document, list) and key.isdigit() and int(key) < len(document):
            document = document[int(key)]
        else:
            return None
    return document


def column_values(gathered: Columns, position: int, dtype: str) -> Any:
    """One gathered column as a NumPy or Pandas array, masking the values that were not found or converted."""
    rows = gathered.rows
    valid = numpy.unpackbits(
        numpy.frombuffer(gathered.validity(position), dtype=numpy.uint8), count=rows, bitorder="little"
    ).astype(bool)
    if dtype == "str":
        return pandas.array(gathered[position], dtype="str")
    if dtype == "bytes":
        return numpy.array(gathered[position], dtype=object)
    values = numpy.asarray(gathered[position])
    if valid.all():
        return values
    masked = pandas.array(values, dtype=NULLABLE_DTYPES[dtype])
    masked[~valid] = pandas.NA
    return masked


class Table:
    """A view of documents as rows and their fields as columns, as `pandas.DataFrame` presents them."""

    def __init__(
        self,
        view: View,
        collection: int | str | None = None,
        *,
        index: Iterable[int] | None = None,
        columns: Iterable[str] | None = None,
        dtypes: Mapping[str, str] | None = None,
    ) -> None:
        """Opens a view of `collection`, over every document and field unless `index` or `columns` narrow it."""
        self.view = view
        self.collection = collection
        self.selected_index = None if index is None else [int(key) for key in index]
        self.selected_columns = None if columns is None else list(columns)
        self.dtypes = dict(dtypes or {})
        unknown = set(self.dtypes.values()) - set(GATHER_TYPES)
        if unknown:
            raise ValueError(f"Unsupported dtypes {sorted(unknown)}, expected one of {list(GATHER_TYPES)}")

    def derive(
        self,
        *,
        index: Iterable[int] | None = None,
        columns: Iterable[str] | None = None,
        dtypes: Mapping[str, str] | None = None,
    ) -> Table:
        """A view of the same collection, narrowed or typed differently."""
        return Table(
            self.view,
            self.collection,
            index=self.selected_index if index is None else index,
            columns=self.selected_columns if columns is None else columns,
            dtypes=self.dtypes if dtypes is None else dtypes,
        )

    # region Shape

    @property
    def index(self) -> list[int]:
        """The keys of the rows, in ascending order unless the view was narrowed to others."""
        if self.selected_index is not None:
            return list(self.selected_index)
        return list(scan_keys(self.view, self.collection))

    @property
    def columns(self) -> list[str]:
        """The fields of the columns, in the order the documents first hold them."""
        if self.selected_columns is not None:
            return list(self.selected_columns)
        found: dict[str, None] = {}
        for page in batched(self.index, PAGE_KEYS):
            found.update(
                dict.fromkeys(column_of(pointer) for pointer in self.view.docs_gist(page, collection=self.collection))
            )
        return list(found)

    def __len__(self) -> int:
        return len(self.index)

    @property
    def shape(self) -> tuple[int, int]:
        return len(self.index), len(self.columns)

    @property
    def size(self) -> int:
        rows, columns = self.shape
        return rows * columns

    @property
    def empty(self) -> bool:
        return 0 in self.shape

    # endregion Shape

    # region Selection

    def astype(self, dtype: str | Mapping[str, str]) -> Table:
        """A view gathering the named columns, or every column, as the given dtypes."""
        typed = dict.fromkeys(self.columns, dtype) if isinstance(dtype, str) else dict(dtype)
        return self.derive(dtypes={**self.dtypes, **typed})

    def __getitem__(self, selection: str | Sequence[str]) -> pandas.Series | Table:
        """One column as a Series, or a view narrowed to several columns."""
        if isinstance(selection, str):
            return self.derive(columns=[selection]).to_pandas()[selection]
        return self.derive(columns=selection)

    def head(self, count: int = 5) -> Table:
        return self.derive(index=self.index[:count])

    def sample(self, count: int) -> Table:
        """A view of up to `count` documents drawn at random by the store."""
        drawn = self.view.sample(count, collection=self.collection)[0]
        return self.derive(index=drawn.tolist())

    # endregion Selection

    # region Exports

    def page_frame(self, keys: list[int], columns: list[str]) -> pandas.DataFrame:
        """The rows `keys` hold, gathered and decoded in one round of calls."""
        data: dict[str, Any] = {}

        typed = [column for column in columns if column in self.dtypes]
        if typed:
            types = [GATHER_TYPES[self.dtypes[column]] for column in typed]
            gathered = self.view.docs_gather(keys, typed, types, collection=self.collection)
            for position, column in enumerate(typed):
                data[column] = column_values(gathered, position, self.dtypes[column])

        untyped = [column for column in columns if column not in self.dtypes]
        if untyped:
            documents = [
                None if document is None else json.loads(bytes(document))
                for document in self.view.docs_read(keys, collection=self.collection)
            ]
            for column in untyped:
                data[column] = [value_at(document, column) for document in documents]

        frame = pandas.DataFrame({column: data[column] for column in columns}, index=pandas.Index(keys, dtype="int64"))
        return frame if columns else pandas.DataFrame(index=pandas.Index(keys, dtype="int64"))

    def to_pandas(self, *, page: int | None = None) -> pandas.DataFrame:
        """Every row as a `DataFrame`, converting `page` rows at a time when a page size is given.

        Only one page of gathered columns and decoded documents is live at a time, so a collection
        larger than memory allows still converts, at the cost of one concatenation at the end.
        """
        keys = self.index
        columns = self.columns
        if page is None:
            return self.page_frame(keys, columns)
        if page < 1:
            raise ValueError(f"A page holds at least one row, got {page}")
        if not keys:
            return self.page_frame(keys, columns)
        return pandas.concat(self.page_frame(list(rows), columns) for rows in batched(keys, page))

    def to_csv(self, path_or_buffer: Any = None, **options: Any) -> str | None:
        return self.to_pandas().to_csv(path_or_buffer, **options)

    def to_json(self, path_or_buffer: Any = None, **options: Any) -> str | None:
        return self.to_pandas().to_json(path_or_buffer, **options)

    def to_parquet(self, path: Any, **options: Any) -> None:
        self.to_pandas().to_parquet(path, **options)

    # endregion Exports

    # region Writes

    def __setitem__(self, column: str, values: Any) -> None:
        """Writes a column into every row: one value per row, or one value broadcast to all of them."""
        keys = self.index
        if isinstance(values, (str, bytes)) or not isinstance(values, Iterable):
            self.view.docs_write(keys, encode_json(values), field=column, collection=self.collection)
            return
        scalars = scalar_column(values)
        if scalars is not None:
            typed, kind = scalars
            if len(typed) != len(keys):
                raise ValueError(f"Got {len(typed)} values for {len(keys)} rows")
            self.view.docs_write(keys, typed, field=column, type=kind, collection=self.collection)
            return
        encoded = [encode_json(value) for value in values]
        if len(encoded) != len(keys):
            raise ValueError(f"Got {len(encoded)} values for {len(keys)} rows")
        self.view.docs_write(keys, encoded, field=column, collection=self.collection)

    def update(self, other: pandas.DataFrame) -> None:
        """Writes the valid cells of `other` into the rows of this view that share their keys."""
        present = set(self.index)
        for column in other.columns:
            series = other[column]
            written = series[series.notna()]
            rows = [int(key) for key in written.index if int(key) in present]
            if not rows:
                continue
            selected = written.loc[rows]
            scalars = scalar_column(selected.to_numpy())
            if scalars is not None:
                typed, kind = scalars
                self.view.docs_write(rows, typed, field=column, type=kind, collection=self.collection)
                continue
            encoded = [encode_json(value) for value in selected]
            self.view.docs_write(rows, encoded, field=column, collection=self.collection)

    def present_rows(self, column: str) -> tuple[list[int], list[memoryview]]:
        """The keys of the rows holding `column`, and the JSON text of the value each holds."""
        keys = self.index
        found = self.view.docs_read(keys, field=column, collection=self.collection)
        pairs = [(key, value) for key, value in zip(keys, found, strict=True) if value is not None]
        return [key for key, _ in pairs], [value for _, value in pairs]

    def drop(self, columns: str | Iterable[str]) -> None:
        """Removes columns from every row holding them."""
        for column in [columns] if isinstance(columns, str) else columns:
            keys, _ = self.present_rows(column)
            if keys:
                self.view.docs_write(keys, None, field=column, collection=self.collection)

    def rename(self, columns: Mapping[str, str]) -> None:
        """Moves the values of every named column under its new name, row by row."""
        for old, new in columns.items():
            keys, values = self.present_rows(old)
            if keys:
                self.view.docs_write(keys, values, field=new, collection=self.collection)
                self.view.docs_write(keys, None, field=old, collection=self.collection)
        if self.selected_columns is not None:
            self.selected_columns = [columns.get(column, column) for column in self.selected_columns]

    def merge_from(self, other: Table) -> None:
        """Merges every document of `other` into the document of the same key here, as RFC 7386 does."""
        keys = other.index
        documents = other.view.docs_read(keys, collection=other.collection)
        pairs = [(key, document) for key, document in zip(keys, documents, strict=True) if document is not None]
        if pairs:
            merged_keys, merged_documents = zip(*pairs, strict=True)
            self.view.docs_write(merged_keys, merged_documents, modification="merge", collection=self.collection)

    # endregion Writes

    # region Construction

    @classmethod
    def from_records(
        cls,
        view: View,
        records: Sequence[Mapping[str, Any]],
        *,
        collection: int | str | None = None,
        index: Iterable[int] | None = None,
    ) -> Table:
        """Writes one document per record, keyed by `index` or by position, and views the collection."""
        keys = list(range(len(records))) if index is None else [int(key) for key in index]
        view.docs_write(keys, [encode_json(dict(record)) for record in records], collection=collection)
        return cls(view, collection)

    @classmethod
    def from_dict(
        cls,
        view: View,
        data: Mapping[str, Sequence[Any]],
        *,
        collection: int | str | None = None,
        index: Iterable[int] | None = None,
    ) -> Table:
        """Writes one document per row of equally long columns, keyed by `index` or by position."""
        rows = {len(values) for values in data.values()}
        if len(rows) > 1:
            raise ValueError(f"Columns hold differing row counts {sorted(rows)}")
        count = rows.pop() if rows else 0
        records = [{column: values[row] for column, values in data.items()} for row in range(count)]
        return cls.from_records(view, records, collection=collection, index=index)

    # endregion Construction
