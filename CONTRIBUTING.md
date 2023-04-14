# Contribution Guide

Thank you for even opening this page!
It's always nice to have third-party contributors!

---

To keep the quality of the code high, we have a set of guidelines common to all Unum projects.

- Whats the procedure? [answered](https://github.com/unum-cloud/awesome/blob/main/Workflow.md#organizing-software-development)
- How to organize branches? [answered](https://github.com/unum-cloud/awesome/blob/main/Workflow.md#branches)
- How to style commits? [answered](https://github.com/unum-cloud/awesome/blob/main/Workflow.md#commits)

## Implementing a new Language Binding

When you start enumerating the dozens of functions across `/include/ustore` header files, you may not immediately want to implement all of them for a new language binding.
We feel the same way, and here is the order in which we approach implementing new language bindings.

1. To open and close DB: `ustore_database_init()`, `ustore_database_free()`.
2. To access binary values in the main collection: `ustore_write()`, `ustore_read()`.
3. Supporting multiple named collections: `ustore_collection_list()`, `ustore_collection_create()`, `ustore_collection_drop()`.
4. Supporting transactions: `ustore_transaction_init()`, `ustore_transaction_stage()`, `ustore_transaction_commit()`, `ustore_transaction_free()`.
5. Supporting scans: `ustore_scan()`.

Once you wrap those functions, we consider the binding usable.
What have we forgotten?

- Machine Learning: `ustore_sample()`.
- Metadata: `ustore_database_control()`, `ustore_measure()`.
- [Graphs](https://unum.cloud/ustore/c/#graphs).
- [Documents](https://unum.cloud/ustore/c/#documents).
- [Paths](https://unum.cloud/ustore/c/#paths).
- [Vectors](https://unum.cloud/ustore/c/#vectors).

All of those are optional and can be implemented in any order, if at all.
Bindings for top-tier languages with compatible build-systems can be integrated into the primary repository.

## Implementing a new Engine

Implementing a new engine is similar to [implementing a new binding](#implementing-a-new-language-binding).
Implementations can be of 2 kinds:

- Lite: native implementation of BLOB modality.
- Complete: native implementation of all modalities.

All existing Open Source implementations are Lite.
This means that the Graph modality logic that comes with RocksDB-based builds is identical to the Graph modality logic that comes with LevelDB-based builds.
So the engine only implements the functions from "db.h" and "blobs.h", and the rest of the logic comes from a generic "modality_graph.cpp", that transforms and redirects calls into BLOB layer.

With this in mind, if you were to implement an engine based on ETCD, FoundationDB, Redis, DragonFly, LMDB, or WiredTiger, you would start with the following functions:

1. To open and close DB: `ustore_database_init()`, `ustore_database_free()`.
2. To access binary values in the main collection: `ustore_write()`, `ustore_read()`.
3. Supporting multiple named collections: `ustore_collection_list()`, `ustore_collection_create()`, `ustore_collection_drop()`.
4. Supporting transactions: `ustore_transaction_init()`, `ustore_transaction_stage()`, `ustore_transaction_commit()`, `ustore_transaction_free()`.
5. Supporting scans: `ustore_scan()`.
6. Machine Learning: `ustore_sample()`. Rarely supported, generally faked via reservoir sampling of bulk scans.
7. Metadata: `ustore_database_control()`, `ustore_measure()`. Can be simply silenced.
8. Memory management: `ustore_arena_free()`, `ustore_error_free()`.

Additionally, you have to configure a few `extern` constants, depending on the range of supported functionality of the underlying engine:

- `bool ustore_supports_transactions_k`.
- `bool ustore_supports_named_collections_k`.
- `bool ustore_supports_snapshots_k`.

For RocksDB, all of those are `true`.
For LevelDB, only the latter.
You should also define the following:

- `ustore_collection_t ustore_collection_main_k`. `0`, by default.
- `ustore_length_t ustore_length_missing_k`. `UINT_MAX`, by default.
- `ustore_key_t ustore_key_unknown_k`. `LONG_MAX`, by default.

> If you are implementing a Lite engine on top of a standalone DBMS, you must acknowledge that logic will split between multiple machines.
> As such, Graph updates generally translate into binary Read-Modify-Write operations.
> If those are forwarded to a remote location, you will pay double the networking costs for two round trips.
