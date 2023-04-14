#!/usr/bin/env bash
rm -rf src/ukv.egg-info ukv.egg-info build CMakeCache.txt \
    && cmake . -DCMAKE_BUILD_TYPE=Debug -DUKV_BUILD_SDK_PYTHON=1 -DUKV_BUILD_ENGINE_UMEM=1 \
    -DPYARROW_DIR="$(python3 -c 'import pyarrow; print(pyarrow.__path__[0])')" \
    && make -j32 py_umem && UKV_DEBUG_PYTHON=0 python3 -m pip install --upgrade --force-reinstall . && pytest --capture=no python/tests/
