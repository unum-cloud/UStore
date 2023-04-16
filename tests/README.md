# Testing UStore

We split tests into:

- Compilation: Validate C++ meta-programming.
- Unit: Short and cover most of the functionality.
- **Stress**: Very long and multithreaded.
- Integration: Check how unrelated features work together.

The in-memory embedded variant is generally used for debugging any non-engine level logic validation.

## Unit Tests

Unit tests are small and generally challenge just one functionality.
Unlike [Stress Tests](#stress-tests), they should be nimble and take only a few seconds to complete, as they are triggered on every single commit.
If you are using VSCode, as we do, the `.vscode` top-level directory already comes with pre-configured launchers for debugging.

```sh
cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DUSTORE_BUILD_BENCHMARKS=0 \
    -DUSTORE_BUILD_TESTS=1 \
    -DUSTORE_BUILD_ENGINE_ROCKSDB=1 \
    -B ./build_release && \
    make -j 12 -C ./build_release
```

Primary unit tests are in one file - [`test_units.cpp`](https://github.com/unum-cloud/ustore/blob/main/tests/test_units.cpp).
Those same tests are used for both embedded and standalone DBMS across all Engines.
You can find a complete list of unit tests [on our website here](https://unum.cloud/ustore/tests/units.html), and you are welcome to contribute.

Here are a few suggestions for implementing unit-tests:

- **Minimalism** is key!
- Remember that `char` easily upcasts to `ustore_key_t`, so prefer single-character keys.
- No test should affect other tests! So if you are testing a persistent DBMS, `.clear()` it in the end.
- If C++ is limiting, remember that you can always use the C standard directly.

## Stress Tests

Stress tests are designed to run infinitely, mostly on RAM disks, doing thousands of transactions concurrently and checking if any corner cases or concurrency and consistency bugs pop up.
The following tests are primarily used:

- Atomicity: Batch-inserting intersecting ranges of keys from different threads and ensuring that all of them pass or fail simultaneously without affecting the global state. You can find it in [`stress_atomicity.cpp`](https://github.com/unum-cloud/ustore/blob/main/tests/stress_atomicity.cpp).
- Linearizability: Validates that transactional writes happen in the proper order by sequentially simulating concurrent transactions and comparing the final state of the DB. You can find it in [`stress_linearizability.cpp`](https://github.com/unum-cloud/ustore/blob/main/tests/stress_linearizability.cpp).

> This only applies to engines, that natively support ACID transactions, excluding LevelDB.

## Integration Tests

Integration tests are mostly implemented in Python.
