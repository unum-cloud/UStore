#!/usr/bin/env bash
rm -rf src/ukv.egg-info ukv.egg-info build CMakeCache.txt \
    && cmake . -DCMAKE_BUILD_TYPE=Release -DUKV_BUILD_SDK_PYTHON=1 -DUKV_BUILD_ENGINE_UMEM=1 \
    && make -j32 py_umem && PYTHON_DEBUG=0 python3.9 -m pip install --upgrade --force-reinstall . && pytest --capture=no python/tests/
