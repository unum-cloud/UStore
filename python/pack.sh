#!/usr/bin/env bash
rm -rf src/ustore.egg-info ustore.egg-info build CMakeCache.txt \
    && cmake . -DCMAKE_BUILD_TYPE=Debug -DUSTORE_BUILD_SDK_PYTHON=1 -DUSTORE_BUILD_ENGINE_UCSET=1 \
    -DPYARROW_DIR="$(python3 -c 'import pyarrow; print(pyarrow.__path__[0])')" \
    && make -j py_ucset && USTORE_DEBUG_PYTHON=0 python3 -m pip install --upgrade --force-reinstall . && pytest --capture=no python/tests/
