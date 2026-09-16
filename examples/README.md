# Examples

Six scripts and one notebook over the pre-v1 Python API, each putting one modality of the database to work.

__None of them run today.__
Every one imports `ustore.smashtable`, the pybind11 layer being removed, and the CPython core in `python/` exposes only its version and the engines it compiled.
They are written against the Wave 4 bindings, so until those land these files are read rather than executed.

## The Examples

| Example                    | What It Shows                             | What It Needs                  |
| :------------------------- | :---------------------------------------- | :----------------------------- |
| `banks_atm_olap.py`        | Batched rows packed into one blob per key | `cudf`, `cuxfilter`, `pyarrow` |
| `banks_atm_olap.ipynb`     | The same store and dashboard, inline      | The same, plus Jupyter         |
| `browser.py`               | Documents, a graph and images in one app  | `streamlit`, `requests`        |
| `community.py`             | Louvain communities over a graph          | `networkx`, `matplotlib`       |
| `consistent_dict.py`       | A `dict` timed against a collection       | `numpy`                        |
| `dataloader.py`            | Collection keys batched into a loader     | `torch`                        |
| `movie_recommendations.py` | Two graphs edited in one transaction      | `networkx`, `requests`         |

Each script opens with a docstring naming what it demonstrates, what it needs installed and how to run it.

## Modalities

`consistent_dict.py` and `dataloader.py` stay on plain binary collections, the first timing a native `dict` against a persistent one across writes, reads, scans and sampling, the second handing a collection straight to a PyTorch `DataLoader` as a map-style dataset.

`community.py` and `movie_recommendations.py` work on graph collections.
The first mirrors Zachary's karate club into the database and partitions it both with the engine's own Louvain and with NetworkX, so the two answers can be compared.
The second walks a follow graph and a views graph to rank unseen movies, then edits both graphs in a single transaction when the user dislikes a recommendation.

`browser.py` renders documents, a graph and images from one database in a Streamlit app, and `banks_atm_olap.py` packs whole batches of ATM withdrawals into fixed-width blobs, one key per batch, then aggregates them on the GPU behind a choropleth map.

## RAPIDS and cuxfilter

cuxfilter reached end of life with RAPIDS 26.06, its final release, and cuSpatial stopped publishing after 25.04.
`banks_atm_olap.yml` therefore pins the whole RAPIDS stack at 26.06 and drops cuSpatial, which the example never used.
Create it with `conda env create -f examples/banks_atm_olap.yml`, and set `channel_priority` from the command line, since a Conda environment file cannot carry it.
