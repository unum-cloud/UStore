#!/usr/bin/env bash
# Brings up an SPDK NVMe/TCP target exporting one RAM-backed namespace without hugepages, or tears it down.
# Usage: USTORE_SPDK_ROOT=<spdk tree> spdk_target.sh up [size_mib] | down | address | serial
# The namespace is 0.25 MiB of 4096-byte blocks by default, the 64-block volume the shared ring cases assume.
set -euo pipefail

root=${USTORE_SPDK_ROOT:?set USTORE_SPDK_ROOT to an SPDK tree with a finished build}
socket=${USTORE_SPDK_SOCKET:-/tmp/ustore-spdk-target.sock}
log=${USTORE_SPDK_LOG:-/tmp/ustore-spdk-target.log}
subsystem_nqn=nqn.2026-09.io.ustore:malloc
serial=USTORE00000000000001
model=USTORE_Malloc
port=4420
size_mib=${2:-0.25}
target=$root/build/bin/nvmf_tgt
rpc="$root/scripts/rpc.py -s $socket"

# WSL2's mirrored networking can leave a loopback listener unreachable, so the docker bridge is the fallback.
candidates() {
    echo 127.0.0.1
    ip -4 -o addr show docker0 2>/dev/null | awk '{print $4}' | cut -d/ -f1
}

reachable() {
    timeout 2 bash -c "exec 3<>/dev/tcp/$1/$port" 2>/dev/null
}

address_of() {
    echo "trtype:TCP adrfam:IPv4 traddr:$1 trsvcid:$port subnqn:$subsystem_nqn"
}

# The main thread is renamed reactor_0, so the target is found by its command line, never by its name.
running() {
    pgrep -f "$target" >/dev/null
}

up() {
    if running; then
        echo "a target is already running; run '$0 down' first" >&2
        exit 1
    fi
    [ -x "$target" ] || { echo "$target is missing; build SPDK first" >&2; exit 1; }
    rm -f "$socket"
    # Without hugepages the environment refuses to start unless -s names its memory; the cpumask locks would
    # refuse a start after an unclean stop.
    "$target" --no-huge -s 512 -m 0x3 --disable-cpumask-locks -r "$socket" >"$log" 2>&1 &
    local attempt
    for attempt in $(seq 1 300); do
        [ -S "$socket" ] && $rpc -t 1 framework_wait_init >/dev/null 2>&1 && break
        running || { cat "$log" >&2; echo "the target died before it listened" >&2; exit 1; }
        sleep 0.1
    done
    $rpc nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192 >&2
    $rpc bdev_malloc_create -b Malloc0 "$size_mib" 4096 >&2
    $rpc nvmf_create_subsystem "$subsystem_nqn" -a -s "$serial" -d "$model" >&2
    $rpc nvmf_subsystem_add_ns "$subsystem_nqn" Malloc0 >&2
    local candidate address=""
    for candidate in $(candidates); do
        $rpc nvmf_subsystem_add_listener "$subsystem_nqn" -t tcp -f ipv4 -a "$candidate" -s "$port" >&2
        if reachable "$candidate"; then
            address=$(address_of "$candidate")
            break
        fi
        echo "a listener on $candidate is unreachable" >&2
    done
    cat "$log" >&2
    [ -n "$address" ] || { echo "no listener is reachable" >&2; down; exit 1; }
    echo "$address"
}

down() {
    if running; then
        pkill -f "$target" || true
        local attempt
        for attempt in $(seq 1 50); do
            running || break
            sleep 0.1
        done
    fi
    rm -f "$socket"
}

address() {
    running || { echo "no target is running" >&2; exit 1; }
    local candidate
    for candidate in $(candidates); do
        if reachable "$candidate"; then
            address_of "$candidate"
            return 0
        fi
    done
    echo "no listener is reachable" >&2
    exit 1
}

case "${1:-}" in
up) up ;;
down) down ;;
address) address ;;
serial) echo "$serial" ;;
*) echo "usage: $0 up [size_mib] | down | address | serial" >&2; exit 2 ;;
esac
