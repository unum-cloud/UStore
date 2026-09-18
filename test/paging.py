"""Paged conversion of a document collection into Pandas, against the frame one unpaged call builds."""

import pytest
from pandas.testing import assert_frame_equal

from ustore.table_view import Table

ROWS = 37

PEOPLE = [{"name": f"person-{row}", "tweets": row * 3, "score": row / 4} for row in range(ROWS)]


@pytest.fixture
def people(view) -> Table:
    return Table.from_records(view, PEOPLE)


@pytest.mark.parametrize("page", [1, 5, ROWS - 1, ROWS, ROWS + 1, 1000])
def test_paged_matches_unpaged(people, page: int):
    """A page size smaller than, equal to or larger than the result answers the same frame."""
    assert_frame_equal(people.to_pandas(page=page), people.to_pandas())


@pytest.mark.parametrize("page", [1, 4, ROWS])
def test_paged_gathers_typed_columns(people, page: int):
    """Typed columns are gathered a page at a time, and the dtypes survive the concatenation."""
    typed = people[["tweets", "score"]].astype({"tweets": "int64", "score": "float64"})
    paged = typed.to_pandas(page=page)
    assert_frame_equal(paged, typed.to_pandas())
    assert [str(dtype) for dtype in paged.dtypes] == ["int64", "float64"]


def test_paged_keeps_the_index_in_order(people):
    """Pages follow the ascending walk of keys, so the concatenated index is the unpaged one."""
    assert people.to_pandas(page=3).index.tolist() == list(range(ROWS))


def test_paged_narrowed_view(people):
    """A view narrowed to some rows pages only those, never the whole collection."""
    narrowed = people.derive(index=[2, 4, 6, 8])
    assert_frame_equal(narrowed.to_pandas(page=3), narrowed.to_pandas())
    assert narrowed.to_pandas(page=3).index.tolist() == [2, 4, 6, 8]


def test_paged_empty_view(view):
    """An empty collection pages into the same empty frame an unpaged call answers."""
    empty = Table(view)
    assert_frame_equal(empty.to_pandas(page=4), empty.to_pandas())


@pytest.mark.parametrize("page", [0, -1])
def test_page_must_hold_a_row(people, page: int):
    """A page holds at least one row, so a page size below one is refused."""
    with pytest.raises(ValueError):
        people.to_pandas(page=page)
