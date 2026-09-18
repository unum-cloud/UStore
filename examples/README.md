# Examples

Six scripts over the v1 Python API, each putting one modality of the database to work.

They target the CPython extension `ustore.lib` and the pure-Python views beside it, so a script runs as soon as the wheel is installed and whatever it draws with is installed too.
`consistent_dict.py`, `community.py` and `movie_recommendations.py` need nothing the project does not already test with.
`dataloader.py` waits on PyTorch, `browser.py` on Streamlit, and `banks_atm_olap.py` on the RAPIDS stack, so those three run only where those are present.

## The Examples

- __`banks_atm_olap.py`__ — batched rows packed into one blob per key, then aggregated on the GPU. Needs `cudf`, `cuxfilter` and `pyarrow`.
- __`browser.py`__ — documents, a graph and images browsed side by side in one app. Needs `streamlit` and `requests`.
- __`community.py`__ — Louvain communities over a graph collection, drawn. Needs `networkx` and `matplotlib`.
- __`consistent_dict.py`__ — a native `dict` timed against a collection over the same keys. Needs `numpy`.
- __`dataloader.py`__ — collection keys batched into a PyTorch loader. Needs `torch`.
- __`movie_recommendations.py`__ — recommendations over two graphs, then both edited in one transaction. Needs `matplotlib` and `requests`.

Each script opens with a docstring naming what it demonstrates, what it needs installed and how to run it.

## Modalities

`consistent_dict.py` and `dataloader.py` stay on plain binary collections.
The first times a native `dict` against a persistent one across three write paths, two read paths, a full scan and the engine's sampling, where `write` given one value broadcasts it across a whole key array.
The second wraps a collection as a map-style dataset for a PyTorch `DataLoader`, with a sampler that draws keys either from `scan_keys` or from the engine's own `sample`.

`community.py` and `movie_recommendations.py` work on graph collections through the NetworkX-compatible views in `ustore.graph_view`.
The first mirrors Zachary's karate club into a graph collection, reads the edges back out and partitions both copies with Louvain, so the two answers can be compared.
The second walks a follow graph and a views graph to rank unseen movies, then edits both graphs in a single transaction when the user dislikes a recommendation, and publishes a movie's document beside its poster blob in another.

`browser.py` renders documents, a graph and images from one database in a Streamlit app, and `banks_atm_olap.py` packs whole batches of ATM withdrawals into fixed-width blobs, one key per batch, then aggregates them on the GPU behind a choropleth map.

## What Has No Equivalent Yet

The engine's own `community_louvain` is gone, so `community.py` partitions with NetworkX over the edges it reads back out of the graph collection.
`movie_recommendations.py` draws its bipartite figure with Matplotlib alone rather than through NetworkX, which it no longer needs for anything else.
There is no media modality, so `browser.py` keeps its photographs in an ordinary binary collection.

## RAPIDS and cuxfilter

cuxfilter reached end of life with RAPIDS 26.06, its final release, and cuSpatial stopped publishing after 25.04.
`banks_atm_olap.yml` therefore pins the whole RAPIDS stack at 26.06 and drops cuSpatial, which the example never used.
Create it with `conda env create -f examples/banks_atm_olap.yml`, and set `channel_priority` from the command line, since a Conda environment file cannot carry it.
