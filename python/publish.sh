#!/usr/bin/env bash

cibuildwheel --platform linux &&
    twine upload wheelhouse/*.whl -u $1 -p $2 --verbose
