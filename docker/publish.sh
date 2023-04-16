#!/bin/bash

version=$(cat VERSION)
docker buildx build . --platform=linux/amd64,linux/arm64 --file docker/Dockerfile --progress=plain -c 32 \
    -t unum/ustore:$version-focal -t unum/ustore:latest --push