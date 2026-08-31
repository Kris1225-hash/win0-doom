#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
    printf 'usage: %s IMAGE.qcow2 GENERATED_VALIDATIONOS.wim\n' "${0##*/}" >&2
    exit 2
fi

image="$(realpath -e -- "$1")"
wim="$(realpath -e -- "$2")"
mount_root="$(mktemp -d /tmp/win0doom-wim-root.XXXXXX)"
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

for command_name in qemu-io qemu-nbd wimlib-imagex ntfs-3g ntfsfix sgdisk; do
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
sudo sgdisk -e "$nbd" >/dev/null
sudo partprobe "$nbd"
udevadm settle
root_partition="$(lsblk -nrpo NAME,LABEL "$nbd" | awk '$2 == "CORESYSTEM" || $2 == "ValidationOS" { print $1; exit }')"
[[ -n "$root_partition" ]] || { printf 'ValidationOS root volume not found.\n' >&2; exit 5; }

sudo ntfsfix "$root_partition" >/dev/null
sudo ntfs-3g "$root_partition" "$mount_root" -o "uid=$(id -u),gid=$(id -g),windows_names"
system_hive="$mount_root/Windows/System32/config/SYSTEM"
[[ -f "$system_hive" ]] || { printf 'stock Win4 SYSTEM hive is missing.\n' >&2; exit 6; }
cp -p -- "$system_hive" "$system_hive.win0doom-win4"
printf 'applying generated ValidationOS image index 1...\n'
sudo wimlib-imagex apply "$wim" 1 "$mount_root"

mv -- "$system_hive" "$system_hive.win0doom-target"
mv -- "$system_hive.win0doom-win4" "$system_hive"

for required in \
    Windows/System32/ccs.exe \
    Windows/System32/CloudCoreInit.exe \
    Windows/System32/apisetschema_win0.dll \
    Windows/System32/f911f154-081b-49de-bbbd-a0dd908085bb_win0-n.dll; do
    if [[ ! -f "$mount_root/$required" ]]; then
        printf 'generated WIM is missing required runlevel-0 payload: %s\n' "$required" >&2
        exit 7
    fi
done

sync -f "$mount_root"
printf 'generated runlevel image applied successfully.\n'
