#!/usr/bin/env bash
#
# Model-checks every Promela model here and stops at the first verdict that is not the expected
# one: `pass` means no assertion fails, `fail` means the model admits what an assertion forbids,
# which is what the `-Dwithout_*` knobs are for. Needs `spin` and a C compiler.

set -eu
cd "$(dirname "$0")"
models=$PWD

command -v spin > /dev/null 2>&1 || {
    echo "  skip  spin is not installed"
    exit 127
}

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

# Each pan holds a hash table of its own, so the search runs under a cgroup cap when systemd is
# reachable and the model shrinks rather than the cap growing.
runner=()
systemd-run --user --scope --quiet -p MemoryMax=8G -p MemorySwapMax=0 -- true > /dev/null 2>&1 &&
    runner=(systemd-run --user --scope --quiet -p MemoryMax=8G -p MemorySwapMax=0 --)

verify() { # verify <model.pml> <expected> <spin defines...>
    local model=$1 expected=$2 verdict=broken
    shift 2
    rm -rf "$build/run"
    mkdir -p "$build/run"
    if (cd "$build/run" && spin -a "$@" "$models/$model" &&
        cc -O2 -DSAFETY -DCOLLAPSE -w -o pan pan.c) > "$build/log" 2>&1; then
        (cd "$build/run" && ${runner[@]+"${runner[@]}"} ./pan -m1000000 -w22) >> "$build/log" 2>&1 || true
        if grep -q "errors: 0" "$build/log"; then verdict=pass
        elif grep -qE "errors: [1-9]" "$build/log"; then verdict=fail
        fi
    fi
    if [ "$verdict" != "$expected" ]; then
        echo "  WRONG $model $* : expected $expected, got $verdict"
        sed -n 1,20p "$build/log"
        exit 1
    fi
    local counts
    counts=$(grep -E "states, stored|depth reached" "$build/log" | tr -s ' \t' ' ' | paste -sd, - || true)
    echo "  ok    $model $* :$counts"
}

echo "durability.pml: the write, flush, record and catalog order, with a crash at every step"
verify durability.pml pass
verify durability.pml pass -Dextents=3
verify durability.pml fail -Dwithout_flush_before_sync
verify durability.pml fail -Dwithout_catalog_sync
