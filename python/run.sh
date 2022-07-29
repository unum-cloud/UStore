#!/usr/bin/env bash
rm -rf build && rm -rf CMakeCache.txt && cmake . && make -j 4 &&
    pip install --upgrade --force-reinstall . && pytest --capture=no python/
