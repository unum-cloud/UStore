# Tools

UStore provides a low-level and a high-level interface to tooling to simplify DevOps and DBMS administration.

- The high-level utilities can be triggered from shell, or Python.
- The low-level utilities are provided as C99 headers:
  - `dataset.h` for bulk dataset imports and exports for industry-standard Parquet, NDJSON and CSV files. 🔜
  - Rolling backups and replication. 🔜
  - Visualization tools and dashboards. 🔜

Tools are built on top of the UStore interface and aren't familiar with the underlying backend implementation.

## Backups and Replications

```python
db.snapshot().save_to('some-path')
db_copy = DataBase('some-path')
```

## Bulk Imports and Exports

```python
db['a'].table.export_same_way_as_pandas('')
db['b'].graph.export_same_way_as_networkx('')
```
