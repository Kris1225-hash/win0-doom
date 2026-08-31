#!/usr/bin/env bash
set -euo pipefail

if (( $# < 1 || $# > 2 )); then
    printf 'usage: %s IMAGE.qcow2 [DOOM1.WAD]\n' "${0##*/}" >&2
    exit 2
fi

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image="$(realpath -e -- "$1")"
wad="${2:-}"
driver="$repo_dir/driver/build/win0doom-display-signed.sys"
doom="$repo_dir/native/build/win0doom.exe"
mount_root="$(mktemp -d /tmp/win0doom-payload-root.XXXXXX)"
nbd=""

disconnect_nbd() {
    local device="$1" server_pid=""
    if [[ -r "/sys/class/block/${device##*/}/pid" ]]; then
        server_pid="$(<"/sys/class/block/${device##*/}/pid")"
    fi
    sudo qemu-nbd --disconnect "$device" >/dev/null 2>&1 || true
    if [[ -n "$server_pid" ]]; then
        for _ in $(seq 1 80); do
            kill -0 "$server_pid" 2>/dev/null || break
            sleep 0.25
        done
    fi
    for _ in $(seq 1 80); do
        qemu-io -f qcow2 -c quit "$image" >/dev/null 2>&1 && return 0
        sleep 0.25
    done
    printf 'the qcow write lock was not released after disconnecting %s.\n' "$device" >&2
    return 1
}

cleanup() {
    sudo umount "$mount_root" >/dev/null 2>&1 || true
    if [[ -n "$nbd" ]]; then
        disconnect_nbd "$nbd" || true
    fi
    rmdir "$mount_root" >/dev/null 2>&1 || true
}
trap cleanup EXIT

for file in "$driver" "$doom"; do
    [[ -f "$file" ]] || { printf 'missing release artifact: %s\n' "$file" >&2; exit 3; }
done
if [[ -n "$wad" ]]; then
    wad="$(realpath -e -- "$wad")"
fi

for command_name in qemu-io qemu-nbd ntfs-3g ntfsfix; do
    command -v "$command_name" >/dev/null || {
        printf 'required command is missing: %s\n' "$command_name" >&2
        exit 3
    }
done

sudo modprobe nbd max_part=16
for candidate in /sys/class/block/nbd*; do
    [[ "${candidate##*/}" =~ ^nbd[0-9]+$ ]] || continue
    if [[ "$(<"$candidate/size")" == 0 ]]; then
        nbd="/dev/${candidate##*/}"
        break
    fi
done
[[ -n "$nbd" ]] || { printf 'no unused nbd device is available.\n' >&2; exit 4; }

sudo qemu-nbd --format=qcow2 --connect="$nbd" "$image"
for _ in $(seq 1 40); do
    [[ "$(sudo blockdev --getsize64 "$nbd" 2>/dev/null || true)" != 0 ]] && break
    sleep 0.25
done
sudo partprobe "$nbd"
udevadm settle
root_partition="$(lsblk -nrpo NAME,LABEL "$nbd" | awk '$2 == "CORESYSTEM" || $2 == "ValidationOS" { print $1; exit }')"
[[ -n "$root_partition" ]] || { printf 'ValidationOS root volume not found.\n' >&2; exit 5; }

sudo ntfsfix "$root_partition" >/dev/null
sudo ntfs-3g "$root_partition" "$mount_root" -o "uid=$(id -u),gid=$(id -g),windows_names"
cp -p -- "$driver" "$mount_root/Windows/System32/drivers/win0doom-display.sys"
cp -p -- "$doom" "$mount_root/Windows/System32/win0doom.exe"
if [[ -n "$wad" ]]; then
    cp -p -- "$wad" "$mount_root/Windows/System32/doom1.wad"
fi
sync -f "$mount_root"
printf 'installed Win0 Doom payload into %s.\n' "$image"
