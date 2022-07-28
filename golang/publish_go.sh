#!/bin/bash

set -e

if [ ! -f ukv_stl.go ]; then
    echo "### Must run from root of go module."
    exit 1
fi

version=$(cat ../VERSION)

echo "### Tidy up"
go mod tidy
echo "### Run Tests"
go test ./...
echo "Module seems okay"

echo "Change branch to origin/main on github"
read -p "Ready (y)? " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted"
    exit 1
fi

echo "Commit changed files related to this release"
read -p "Ready (y)? " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted"
    exit 1
fi

echo "### Taging your commit with $version" &&
    git tag $version &&
    echo "### Pushing" &&
    git push origin $version &&
    echo "### Done"
