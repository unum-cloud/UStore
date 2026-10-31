#!/usr/bin/env bash
# Checks that HEAD is the release commit for a tag and that every version site carries its version.
#
#     .github/scripts/check-release.sh v0.13.13
set -euo pipefail
tag=$1 version=${1#v}
subject=$(git log -1 --format=%s)
if [[ ! $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$ ]]; then
    echo "check-release: a release tag is exactly 'vX.Y.Z' or 'vX.Y.Z-rc.N', got '$tag'"
    exit 1
fi
if [ "$subject" != "Release: $tag" ]; then
    echo "check-release: $tag must sit on 'Release: $tag', but HEAD is '$subject'"
    exit 1
fi
stale=""
[ "$(cat VERSION)" = "$version" ] || stale="$stale VERSION"
grep -qxF "version: $version" CITATION.cff || stale="$stale CITATION.cff"
if [ -n "$stale" ]; then
    echo "check-release: these files don't carry $version:$stale"
    exit 1
fi
