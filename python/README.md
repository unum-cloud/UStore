# Python SDK

The bindings are pure CPython in C11, sitting directly on the C ABI in `include/ustore/`, with no pybind11, NanoBind or SWIG.
The native core is one extension, `ustore.lib`, and the `ustore` package re-exports it.

## Installation

```sh
git submodule update --init --recursive
pip install .
python -c "import ustore, ustore.lib; print(ustore.lib.__version__, ustore.lib.engines_compiled())"
```

`pyproject.toml` builds through `scikit-build-core`, which drives the project's own CMake tree, so there is no `setup.py`.
The wheel compiles the SmashTable engine alone by default.
RocksDB and LevelDB are enabled through the standard `CMAKE_ARGS` or through scikit-build config settings, never through variables of UStore's own:

```sh
CMAKE_ARGS="-DUSTORE_USE_ROCKSDB=ON -DUSTORE_USE_LEVELDB=ON" pip install .
pip install . --config-settings=cmake.define.USTORE_USE_ROCKSDB=ON
```

`cibuildwheel` builds wheels for CPython 3.12, 3.13 and 3.14, and for the free-threaded 3.14t, where importing `ustore` keeps the GIL disabled.

## Layout

| Path                 | What it holds                                                   |
| :------------------- | :-------------------------------------------------------------- |
| `lib.c`              | The module glue of `ustore.lib`                                 |
| `CMakeLists.txt`     | The `ustore_python` target, installed into the `ustore` package |
| `ustore/__init__.py` | The package, re-exporting `ustore.lib`                          |
| `ustore/lib.pyi`     | Type stubs for the extension                                    |
| `ustore/py.typed`    | The PEP 561 marker that ships the stubs                         |

Each modality joins as one C file beside `lib.c`, together with its own `.pyi` stub in `ustore/`.
The suites live in `tests/` beside the C++ ones, benchmarks in `bench/`, and examples in `examples/`.

## Today's Interface

Until the Wave 4 bindings land, the extension exposes only its version and the engines it compiled:

```python
import ustore.lib

ustore.lib.__version__        # "0.13.12"
ustore.lib.engines_compiled() # ("smashtable",)
```

Collections, transactions, batch operations, and the NetworkX-like and Pandas-like views arrive with those bindings.
The Python suites in `tests/` and the scripts in `examples/` still target the pre-v1 API, so the suites skip themselves until they are ported.
