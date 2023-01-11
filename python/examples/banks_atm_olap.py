"""
Example of building an application-specific OLAP DBMS on top of binary collections of UKV.

Imagine owning a big bank with thousands of ATMs all across the US.
You are aggregating the withdrawals and, once it happens, count the number of 
people in front of the embedded camera. You also count those at other times.

## Usage

Use The provided `banks_atm_olap.yml` file for the Conda environment configuration.
It will bring all the needed NVidia libraries: `conda env create -f python/examples/banks_atm_olap.yml`.

## Links

Row to Columnar Conversion in Arrow: 
https://arrow.apache.org/docs/cpp/examples/row_columnar_conversion.html
"""
import struct
from typing import Tuple
import requests

import geojson
import pyarrow as pa
import numpy as np
import pandas as pd

import cudf
import cuxfilter as cux
from cuxfilter import charts
from bokeh.server.server import Server
from bokeh.document.document import Document

# import ukv.umem as ukv

MAPBOX_API_KEY = 'pk.eyJ1IjoiYXRob3J2ZSIsImEiOiJjazBmcmlhYzAwYXc2M25wMnE3bGttbzczIn0.JCDQliLc-XTU52iKa8L8-Q'
GEOJSON_URL = 'https://raw.githubusercontent.com/rapidsai/cuxfilter/GTC-2018-mortgage-visualization/javascript/demos/GTC%20demo/public/data/zip3-ms-rhs-lessprops.json'

Measurement = Tuple[
    int,  # ids - ignored when packaging
    int,  # timestamps
    float,  # latitudes
    float,  # longitudes
    float,  # amounts
    int,  # humans
    int,  # zips
]
measurement_format: str = 'Qfffii'

zip_codes_geojson: str = geojson.loads(requests.get(GEOJSON_URL).text)
zip_codes: list[int] = [int(x['properties']['ZIP3'])
                        for x in zip_codes_geojson['features']]


def generate_rows(
    batch_size: int = 10_000,
    city_center=(40.177200, 44.503490),
    radius_degrees: float = 0.005,
    start_time: int = 1669996348,
) -> pd.DataFrame:
    seconds_between_events = 60
    return pd.DataFrame({
        'ids': np.random.randint(low=1, high=None, size=batch_size),
        'timestamps': np.arange(start_time, start_time + batch_size * seconds_between_events, seconds_between_events, dtype=int),
        'latitudes': np.random.uniform(city_center[0] - radius_degrees, city_center[0] + radius_degrees, [batch_size]).astype(np.single),
        'longitudes': np.random.uniform(city_center[1] - radius_degrees, city_center[1] + radius_degrees, [batch_size]).astype(np.single),
        'amounts': np.random.lognormal(3., 1., [batch_size]).astype(np.single) * 100.0,
        'humans': np.random.randint(low=1, high=4, size=batch_size),
        'zips': np.random.choice(zip_codes, size=batch_size),
    })


def dump_rows(measurements: pd.DataFrame) -> pa.FixedSizeBinaryArray:
    count_rows = len(measurements)
    bytes_per_row = struct.calcsize(measurement_format)
    bytes_for_rows = bytearray(bytes_per_row * count_rows)
    timestamps = measurements['timestamps']
    latitudes = measurements['latitudes']
    longitudes = measurements['longitudes']
    amounts = measurements['amounts']
    humans = measurements['humans']
    zips = measurements['zips']
    for row_idx in range(count_rows):
        struct.pack_into(
            measurement_format, bytes_for_rows, bytes_per_row * row_idx,
            timestamps[row_idx], latitudes[row_idx], longitudes[row_idx],
            amounts[row_idx], humans[row_idx], zips[row_idx],
        )

    # https://arrow.apache.org/docs/python/generated/pyarrow.FixedSizeBinaryType.html#pyarrow.FixedSizeBinaryType
    datatype = pa.binary(bytes_per_row)
    # https://arrow.apache.org/docs/python/generated/pyarrow.FixedSizeBinaryArray.html#pyarrow.FixedSizeBinaryArray.from_buffers
    buffers = [None, pa.py_buffer(bytes_for_rows)]
    return pa.FixedSizeBinaryArray.from_buffers(datatype, count_rows, buffers, null_count=0)


def parse_rows(bytes_for_rows: bytes) -> pd.DataFrame:

    if isinstance(bytes_for_rows, pa.FixedSizeBinaryArray):
        bytes_for_rows = bytes_for_rows.buffers()[1].to_pybytes()

    bytes_per_row = struct.calcsize(measurement_format)
    count_rows = len(bytes_for_rows) // bytes_per_row
    timestamps = np.zeros((count_rows), dtype=np.int64)
    latitudes = np.zeros((count_rows), dtype=np.single)
    longitudes = np.zeros((count_rows), dtype=np.single)
    amounts = np.zeros((count_rows), dtype=np.single)
    humans = np.zeros((count_rows), dtype=np.int32)
    zips = np.zeros((count_rows), dtype=np.int32)
    for row_idx in range(count_rows):
        row = struct.unpack_from(
            measurement_format, bytes_for_rows, bytes_per_row * row_idx)
        timestamps[row_idx] = row[0]
        latitudes[row_idx] = row[1]
        longitudes[row_idx] = row[2]
        amounts[row_idx] = row[3]
        humans[row_idx] = row[4]
        zips[row_idx] = row[5]

    return pd.DataFrame({
        'timestamps': timestamps,
        'latitudes': latitudes,
        'longitudes': longitudes,
        'amounts': amounts,
        'humans': humans,
        'zips': zips,
    })


def rolling_mean_tempretures(amounts, window_size=20):
    """
        Computes an array of local averages with a gliding window.
        https://stackoverflow.com/a/30141358
    """
    pd.Series(amounts).rolling(
        window=window_size).mean().iloc[window_size-1:].values


def make_dashboard(doc: Document):

    # Preprocess for visualizations
    df: pd.DataFrame = generate_rows(100)
    df['time'] = pd.to_datetime(df['timestamps'], unit='s')
    cudf_df = cudf.from_pandas(df)
    cux_df = cux.DataFrame.from_dataframe(cudf_df)

    chart_map = charts.choropleth(
        x='zips',
        elevation_column='humans',
        elevation_aggregate_fn='mean',
        color_column='amounts',
        color_aggregate_fn='mean',
        mapbox_api_key=MAPBOX_API_KEY,
        geoJSONSource=zip_codes_geojson_url,
        title='ATM Withdrawals Across the US',
    )

    chart_humans = charts.bokeh.bar('humans')
    slider_time = charts.date_range_slider(
        'time',
        title='Timeframe',
    )
    overall_amounts = charts.number(
        x='amounts',
        aggregate_fn='sum',
        format='${value:,.1f}',
        title='Total Withdrawals',
    )
    overall_humans = charts.number(
        x='humans',
        aggregate_fn='sum',
        format='{value:,.0f}',
        title='Humans Detected',
    )

    d = cux_df.dashboard(
        [chart_map],
        layout_array=[[1]],
        sidebar=[slider_time, overall_amounts, overall_humans],
        theme=cux.themes.rapids,
        title='Map of ATMs',
    )

    # run the dashboard as a webapp:
    d._dashboard.generate_dashboard(
        d.title, d._charts, d._theme
    ).server_doc(doc)


if __name__ == '__main__':

    tmp = generate_rows(100)
    arrow_array = dump_rows(tmp)
    recovered = parse_rows(arrow_array)

    # Validate the serialization procedure
    assert tmp['timestamps'].equals(recovered['timestamps'])
    assert tmp['latitudes'].equals(recovered['latitudes'])
    assert tmp['longitudes'].equals(recovered['longitudes'])
    assert tmp['amounts'].equals(recovered['amounts'])

    # Using persistent store will be identical to using this
    # db = ukv.DataBase()
    # measurements = db.main
    measurements: dict[int, bytes] = {}

    server = Server(
        make_dashboard,
        num_procs=1,
        allow_websocket_origin=['*'],
        check_unused_sessions_milliseconds=500,
    )
    server.start()

    print(f'running server on port {80}')
    server.io_loop.start()
