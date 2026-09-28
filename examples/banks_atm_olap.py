"""Builds an application-specific OLAP store for ATM withdrawals on binary UStore collections.

Imagine a bank with thousands of ATMs across the US.
Each withdrawal records when and where it happened, how much was taken, and how many people the
embedded camera saw standing in front of the machine.

Rather than one key per withdrawal, a whole batch of rows is packed into one fixed-width binary
blob and stored under a single key, which is what makes this an OLAP layout rather than an OLTP
one: the analytical pass wants long runs of columns, not scattered point lookups.
`struct` lays the rows out, Arrow wraps the same buffer without copying it, and reading a batch
back is one blob fetch followed by one unpack.

The dashboard aggregates the recovered rows on the GPU with cuDF and draws withdrawals by zip code
on a choropleth map, with a timeframe slider and running totals beside it.

Needs `cudf`, `cuxfilter`, `pyarrow`, `geojson`, `requests` and the `ustore` bindings.
cuxfilter reached end of life with RAPIDS 26.06, its final release, so the whole RAPIDS stack has to
be pinned there; `banks_atm_olap.yml` does that.
Run with `python examples/banks_atm_olap.py`, then open the printed port.

Row to columnar conversion in Arrow:
https://arrow.apache.org/docs/cpp/examples/row_columnar_conversion.html
"""

import struct
from functools import partial

import cudf
import cuxfilter as cux
import geojson
import numpy as np
import pandas as pd
import pyarrow as pa
import requests
from bokeh.document.document import Document
from bokeh.server.server import Server
from cuxfilter import charts

import ustore
from ustore.keys import scan_keys

MAPBOX_API_KEY = "pk.eyJ1IjoiYXRob3J2ZSIsImEiOiJjazBmcmlhYzAwYXc2M25wMnE3bGttbzczIn0.JCDQliLc-XTU52iKa8L8-Q"
GEOJSON_LINK = "https://raw.githubusercontent.com/rapidsai/cuxfilter/GTC-2018-mortgage-visualization/javascript/demos/GTC%20demo/public/data/zip3-ms-rhs-lessprops.json"

# Timestamp, latitude, longitude, amount, humans, zip code. The row identifier is deliberately left
# out: it identifies a withdrawal, and nothing in the analytical pass groups by it.
MEASUREMENT_FORMAT = "Qfffii"

CITY_CENTER = (40.177200, 44.503490)
RADIUS_DEGREES = 0.005
SECONDS_BETWEEN_EVENTS = 60
START_TIME = 1669996348
ROWS_PER_BATCH = 1_000
BATCH_COUNT = 10
ROLLING_WINDOW = 20
HIGHEST_IDENTIFIER = 2**31
REQUEST_TIMEOUT_SECONDS = 30
SERVER_PORT = 80

Measurement = tuple[int, float, float, float, int, int]
"""One unpacked row, in the order `MEASUREMENT_FORMAT` lays it out."""


def load_zip_codes() -> list[int]:
    """Reads the three-digit zip codes out of the polygon set the choropleth draws.

    The map itself fetches the polygons again by URL, so only the codes are kept here, to draw
    withdrawals from the same zip codes the map can render.
    """
    polygons = geojson.loads(requests.get(GEOJSON_LINK, timeout=REQUEST_TIMEOUT_SECONDS).text)
    return [int(feature["properties"]["ZIP3"]) for feature in polygons["features"]]


def generate_rows(zip_codes: list[int], batch_size: int = 10_000, start_time: int = START_TIME) -> pd.DataFrame:
    """Synthesizes one batch of withdrawals, scattered around the city center at a fixed cadence.

    Amounts are lognormal, so a few large withdrawals sit in a long tail of small ones, and every
    column carries the width `MEASUREMENT_FORMAT` packs, so a round trip recovers it unchanged.
    """
    generator = np.random.default_rng(start_time)
    return pd.DataFrame(
        {
            "ids": generator.integers(low=1, high=HIGHEST_IDENTIFIER, size=batch_size),
            "timestamps": np.arange(
                start_time,
                start_time + batch_size * SECONDS_BETWEEN_EVENTS,
                SECONDS_BETWEEN_EVENTS,
                dtype=np.int64,
            ),
            "latitudes": generator.uniform(
                CITY_CENTER[0] - RADIUS_DEGREES,
                CITY_CENTER[0] + RADIUS_DEGREES,
                batch_size,
            ).astype(np.single),
            "longitudes": generator.uniform(
                CITY_CENTER[1] - RADIUS_DEGREES,
                CITY_CENTER[1] + RADIUS_DEGREES,
                batch_size,
            ).astype(np.single),
            "amounts": (generator.lognormal(3.0, 1.0, batch_size) * 100.0).astype(np.single),
            "humans": generator.integers(low=1, high=4, size=batch_size, dtype=np.int32),
            "zips": generator.choice(zip_codes, size=batch_size).astype(np.int32),
        }
    )


def pack_rows(measurements: pd.DataFrame) -> bytearray:
    """Packs the frame into one fixed-width buffer, row after row, dropping the identifier column."""
    bytes_per_row = struct.calcsize(MEASUREMENT_FORMAT)
    packed = bytearray(bytes_per_row * len(measurements))
    columns = (
        measurements["timestamps"],
        measurements["latitudes"],
        measurements["longitudes"],
        measurements["amounts"],
        measurements["humans"],
        measurements["zips"],
    )
    for row_index in range(len(measurements)):
        struct.pack_into(
            MEASUREMENT_FORMAT,
            packed,
            bytes_per_row * row_index,
            *(column[row_index] for column in columns),
        )
    return packed


def dump_rows(measurements: pd.DataFrame) -> pa.FixedSizeBinaryArray:
    """Wraps the packed rows as an Arrow array, which borrows the buffer instead of copying it."""
    packed = pack_rows(measurements)
    datatype = pa.binary(struct.calcsize(MEASUREMENT_FORMAT))
    buffers = [None, pa.py_buffer(packed)]
    return pa.FixedSizeBinaryArray.from_buffers(datatype, len(measurements), buffers, null_count=0)


def unpack_row(packed: bytes, row_index: int) -> Measurement:
    """Reads one row out of the packed buffer, by offset rather than by slicing it."""
    bytes_per_row = struct.calcsize(MEASUREMENT_FORMAT)
    return struct.unpack_from(MEASUREMENT_FORMAT, packed, bytes_per_row * row_index)


def parse_rows(packed: bytes | pa.FixedSizeBinaryArray) -> pd.DataFrame:
    """Unpacks a stored blob back into a frame, accepting either raw bytes or the Arrow array."""
    if isinstance(packed, pa.FixedSizeBinaryArray):
        packed = packed.buffers()[1].to_pybytes()

    count_rows = len(packed) // struct.calcsize(MEASUREMENT_FORMAT)
    timestamps = np.zeros(count_rows, dtype=np.int64)
    latitudes = np.zeros(count_rows, dtype=np.single)
    longitudes = np.zeros(count_rows, dtype=np.single)
    amounts = np.zeros(count_rows, dtype=np.single)
    humans = np.zeros(count_rows, dtype=np.int32)
    zips = np.zeros(count_rows, dtype=np.int32)

    for row_index in range(count_rows):
        row = unpack_row(packed, row_index)
        timestamps[row_index] = row[0]
        latitudes[row_index] = row[1]
        longitudes[row_index] = row[2]
        amounts[row_index] = row[3]
        humans[row_index] = row[4]
        zips[row_index] = row[5]

    return pd.DataFrame(
        {
            "timestamps": timestamps,
            "latitudes": latitudes,
            "longitudes": longitudes,
            "amounts": amounts,
            "humans": humans,
            "zips": zips,
        }
    )


def rolling_mean_amounts(amounts, window_size: int = ROLLING_WINDOW) -> np.ndarray:
    """Smooths the withdrawal amounts with a gliding window, dropping the incomplete leading window."""
    rolling = pd.Series(amounts).rolling(window=window_size).mean()
    return rolling.iloc[window_size - 1 :].to_numpy()


def check_round_trip(zip_codes: list[int]) -> None:
    """Asserts that packing and unpacking a batch preserves every column it carries."""
    original = generate_rows(zip_codes, ROWS_PER_BATCH)
    recovered = parse_rows(dump_rows(original))
    for column in ("timestamps", "latitudes", "longitudes", "amounts", "humans", "zips"):
        assert original[column].equals(recovered[column]), f"{column} did not survive the round trip"


def store_batches(database: ustore.Database, zip_codes: list[int]) -> None:
    """Writes each batch as one blob, keyed by batch index rather than by withdrawal."""
    for batch_index in range(BATCH_COUNT):
        start_time = START_TIME + batch_index * ROWS_PER_BATCH * SECONDS_BETWEEN_EVENTS
        rows = generate_rows(zip_codes, ROWS_PER_BATCH, start_time)
        database.set(batch_index, bytes(pack_rows(rows)))


def load_measurements(database: ustore.Database) -> pd.DataFrame:
    """Reads every stored blob back as one batch and concatenates them into one frame."""
    keys = list(scan_keys(database))
    stored = database.read(keys)
    return pd.concat([parse_rows(bytes(blob)) for blob in stored], ignore_index=True)


def make_dashboard(doc: Document, measurements: pd.DataFrame) -> None:
    """Builds the cuxfilter dashboard over the recovered rows and attaches it to a Bokeh document."""
    frame = measurements.copy()
    frame["time"] = pd.to_datetime(frame["timestamps"], unit="s")
    cux_frame = cux.DataFrame.from_dataframe(cudf.from_pandas(frame))

    chart_map = charts.choropleth(
        x="zips",
        elevation_column="humans",
        elevation_aggregate_fn="mean",
        color_column="amounts",
        color_aggregate_fn="mean",
        mapbox_api_key=MAPBOX_API_KEY,
        geoJSONSource=GEOJSON_LINK,
        title="ATM Withdrawals Across the US",
    )
    chart_humans = charts.bar("humans")
    slider_time = charts.date_range_slider("time", title="Timeframe")
    overall_amounts = charts.number(
        expression="amounts",
        aggregate_fn="sum",
        format="${value:,.1f}",
        title="Total Withdrawals",
    )
    overall_humans = charts.number(
        expression="humans",
        aggregate_fn="sum",
        format="{value:,.0f}",
        title="Humans Detected",
    )

    dashboard = cux_frame.dashboard(
        [chart_map, chart_humans],
        layout_array=[[1, 2]],
        sidebar=[slider_time, overall_amounts, overall_humans],
        theme=cux.themes.rapids,
        title="Map of ATMs",
    )

    # cuxfilter exposes no public servable, so the documented multi-user recipe reaches through to
    # the generated layout and hands it the Bokeh document.
    dashboard._dashboard.generate_dashboard(
        dashboard.title,
        dashboard._charts,
        dashboard._theme,
    ).server_doc(doc)


def main() -> None:
    """Stores the synthesized batches, reads them back and serves the dashboard over them."""
    zip_codes = load_zip_codes()
    check_round_trip(zip_codes)

    with ustore.Database.open_ram() as database:
        store_batches(database, zip_codes)
        recovered = load_measurements(database)

    smoothed = rolling_mean_amounts(recovered["amounts"])
    print(f"Recovered {len(recovered)} withdrawals, smoothed into {len(smoothed)} windowed means")

    server = Server(
        partial(make_dashboard, measurements=recovered),
        num_procs=1,
        allow_websocket_origin=["*"],
        check_unused_sessions_milliseconds=500,
    )
    server.start()
    print(f"running server on port {SERVER_PORT}")
    server.io_loop.start()


if __name__ == "__main__":
    main()
