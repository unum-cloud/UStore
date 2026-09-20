#!/usr/bin/env bash
# Brings up a RAM-backed NVMe namespace through nvmet's loop transport, or tears it down.
# Usage: nvmet_loop.sh up [size] | down | device
# The namespace is a tmpfs file, so it reports 4096-byte logical blocks and enforces every alignment rule.
set -euo pipefail

configfs=/sys/kernel/config/nvmet
subsystem_nqn=nqn.2026-09.io.ustore:ramloop
image=/dev/shm/ustore-nvmet.img
size=${2:-1G}

# The kernel scans namespaces asynchronously, so the block device is found by its NSID, never by name.
device_for_nsid() {
    local wanted=$1 candidate
    for candidate in /sys/block/nvme*n*; do
        [[ $(basename "$candidate") =~ ^nvme[0-9]+n[0-9]+$ ]] || continue
        [ -e "$candidate/nsid" ] || continue
        if [ "$(cat "$candidate/nsid")" = "$wanted" ] && grep -q "$subsystem_nqn" "$candidate/device/subsysnqn" 2>/dev/null; then
            echo "/dev/$(basename "$candidate")"
            return 0
        fi
    done
    return 1
}

up() {
    sudo modprobe nvmet nvme-loop
    truncate -s "$size" "$image"
    sudo mkdir -p "$configfs/subsystems/$subsystem_nqn/namespaces/1"
    echo 1 | sudo tee "$configfs/subsystems/$subsystem_nqn/attr_allow_any_host" >/dev/null
    echo "$image" | sudo tee "$configfs/subsystems/$subsystem_nqn/namespaces/1/device_path" >/dev/null
    echo 1 | sudo tee "$configfs/subsystems/$subsystem_nqn/namespaces/1/enable" >/dev/null
    sudo mkdir -p "$configfs/ports/1"
    echo loop | sudo tee "$configfs/ports/1/addr_trtype" >/dev/null
    sudo ln -sf "$configfs/subsystems/$subsystem_nqn" "$configfs/ports/1/subsystems/$subsystem_nqn"
    echo "transport=loop,nqn=$subsystem_nqn" | sudo tee /dev/nvme-fabrics >/dev/null
    local device="" attempt
    for attempt in $(seq 1 50); do
        device=$(device_for_nsid 1) && break
        sleep 0.1
    done
    [ -n "$device" ] || { echo "the namespace never appeared under /sys/block" >&2; exit 1; }
    sudo chown "$(id -un)" "$device"
    echo "$device"
}

down() {
    local controller
    for controller in /sys/class/nvme/nvme*; do
        [ -e "$controller/subsysnqn" ] || continue
        if grep -q "$subsystem_nqn" "$controller/subsysnqn"; then
            echo 1 | sudo tee "$controller/delete_controller" >/dev/null
        fi
    done
    sudo rm -f "$configfs/ports/1/subsystems/$subsystem_nqn"
    sudo rmdir "$configfs/ports/1" 2>/dev/null || true
    echo 0 | sudo tee "$configfs/subsystems/$subsystem_nqn/namespaces/1/enable" >/dev/null 2>&1 || true
    sudo rmdir "$configfs/subsystems/$subsystem_nqn/namespaces/1" 2>/dev/null || true
    sudo rmdir "$configfs/subsystems/$subsystem_nqn" 2>/dev/null || true
    rm -f "$image"
}

case "${1:-}" in
up) up ;;
down) down ;;
device) device_for_nsid 1 ;;
*) echo "usage: $0 up [size] | down | device" >&2; exit 2 ;;
esac
