#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s IMAGE.qcow2 [DOOM1.WAD]\n' "${0##*/}" >&2
}

if (( $# < 1 || $# > 2 )); then
    usage
    exit 2
fi

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image="$(realpath -e -- "$1")"
payload="$repo_dir/tools/enable-test-signing.cmd"
driver="$repo_dir/driver/build/win0doom-display-signed.sys"
doom="$repo_dir/native/build/win0doom.exe"
runlevel_reg="$repo_dir/driver/install-win0-runlevel.reg"
driver_reg="$repo_dir/driver/install-win0doom-display.reg"
wad="${2:-}"
mount_root="$(mktemp -d /tmp/win0doom-prep-root.XXXXXX)"
mount_efi="$(mktemp -d /tmp/win0doom-prep-efi.XXXXXX)"
runtime="$(mktemp -d /tmp/win0doom-prep-vm.XXXXXX)"
bcd_patch="$(mktemp /tmp/win0doom-onecore-bcd.XXXXXX.reg)"
runlevel_target_reg="$(mktemp /tmp/win0doom-runlevel-target.XXXXXX.reg)"
driver_target_reg="$(mktemp /tmp/win0doom-driver-target.XXXXXX.reg)"
nbd=""
qemu_pid=""

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
    sudo umount "$mount_efi" >/dev/null 2>&1 || true
    if [[ -n "$nbd" ]]; then
        disconnect_nbd "$nbd" || true
    fi
    if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
        kill "$qemu_pid" 2>/dev/null || true
    fi
    rmdir "$mount_root" "$mount_efi" >/dev/null 2>&1 || true
    rm -rf -- "$runtime"
    rm -f -- "$bcd_patch"
    rm -f -- "$runlevel_target_reg" "$driver_target_reg"
}
trap cleanup EXIT

for required_file in "$payload" "$driver" "$doom" "$runlevel_reg" "$driver_reg"; do
    if [[ ! -f "$required_file" ]]; then
        printf 'required release artifact is missing: %s\n' "$required_file" >&2
        printf 'run tools/build-release.sh first.\n' >&2
        exit 3
    fi
done
if [[ -n "$wad" ]]; then
    wad="$(realpath -e -- "$wad")"
fi

for required_command in qemu-img qemu-io qemu-nbd qemu-system-x86_64 socat ntfs-3g ntfsfix; do
    command -v "$required_command" >/dev/null || {
        printf 'required command is missing: %s\n' "$required_command" >&2
        exit 3
    }
done

ovmf_code=""
ovmf_vars=""
for candidate in /usr/share/edk2/OvmfX64/OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE.fd; do
    [[ -f "$candidate" ]] && ovmf_code="$candidate" && break
done
for candidate in /usr/share/edk2/OvmfX64/OVMF_VARS.fd /usr/share/OVMF/OVMF_VARS.fd; do
    [[ -f "$candidate" ]] && ovmf_vars="$candidate" && break
done
if [[ -z "$ovmf_code" || -z "$ovmf_vars" ]]; then
    printf 'could not find OVMF_CODE.fd and OVMF_VARS.fd.\n' >&2
    exit 4
fi

attach_image() {
    sudo modprobe nbd max_part=16
    for candidate in /sys/class/block/nbd*; do
        [[ "${candidate##*/}" =~ ^nbd[0-9]+$ ]] || continue
        if [[ "$(<"$candidate/size")" == 0 ]]; then
            nbd="/dev/${candidate##*/}"
            break
        fi
    done
    [[ -n "$nbd" ]] || { printf 'no unused nbd device is available.\n' >&2; exit 5; }

    sudo qemu-nbd --format=qcow2 --connect="$nbd" "$image"
    for _ in $(seq 1 40); do
        [[ "$(sudo blockdev --getsize64 "$nbd" 2>/dev/null || true)" != 0 ]] && break
        sleep 0.25
    done
    if [[ "$(sudo blockdev --getsize64 "$nbd" 2>/dev/null || true)" == 0 ]]; then
        printf 'qemu-nbd connected, but the kernel never published the image size.\n' >&2
        exit 6
    fi
    sudo partprobe "$nbd"
    udevadm settle
}

mount_validationos() {
    local root_partition efi_partition
    root_partition="$(lsblk -nrpo NAME,LABEL "$nbd" | awk '$2 == "CORESYSTEM" || $2 == "ValidationOS" { print $1; exit }')"
    efi_partition="$(lsblk -nrpo NAME,LABEL "$nbd" | awk '$2 == "SYSTEM" { print $1; exit }')"
    [[ -n "$root_partition" && -n "$efi_partition" ]] || {
        printf 'the image does not contain the expected ValidationOS and SYSTEM volumes.\n' >&2
        exit 7
    }
    sudo ntfsfix "$root_partition" >/dev/null
    sudo ntfs-3g "$root_partition" "$mount_root" \
        -o "uid=$(id -u),gid=$(id -g),windows_names"
    sudo mount -o "uid=$(id -u),gid=$(id -g),umask=022" "$efi_partition" "$mount_efi"
}

detach_image() {
    sync -f "$mount_root"
    sync -f "$mount_efi"
    sudo umount "$mount_efi"
    sudo umount "$mount_root"
    disconnect_nbd "$nbd"
    nbd=""
}

attach_image
mount_validationos
sed 's/HKEY_LOCAL_MACHINE\\SYSTEM/HKEY_LOCAL_MACHINE\\VOS_SYSTEM/g' \
    "$runlevel_reg" > "$runlevel_target_reg"
sed 's/HKEY_LOCAL_MACHINE\\SYSTEM/HKEY_LOCAL_MACHINE\\VOS_SYSTEM/g' \
    "$driver_reg" > "$driver_target_reg"
cp -p -- "$payload" "$mount_root/Windows/System32/w.cmd"
cp -p -- "$driver" "$mount_root/Windows/System32/drivers/win0doom-display.sys"
cp -p -- "$doom" "$mount_root/Windows/System32/win0doom.exe"
cp -p -- "$runlevel_reg" "$mount_root/Windows/System32/win0doom-runlevel.reg"
cp -p -- "$driver_reg" "$mount_root/Windows/System32/win0doom-driver.reg"
cp -p -- "$runlevel_target_reg" "$mount_root/Windows/System32/win0doom-runlevel-target.reg"
cp -p -- "$driver_target_reg" "$mount_root/Windows/System32/win0doom-driver-target.reg"
if [[ -n "$wad" ]]; then
    cp -p -- "$wad" "$mount_root/Windows/System32/doom1.wad"
fi
rm -f -- "$mount_root/Windows/System32/win0doom-bcd-ready.txt"
detach_image

cp -- "$ovmf_vars" "$runtime/OVMF_VARS.fd"
monitor="$runtime/monitor.sock"
pidfile="$runtime/qemu.pid"
qemu-system-x86_64 \
    -name win0doom-bcd-preparation \
    -machine q35,accel=kvm -cpu host -smp 2 -m 2G \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$runtime/OVMF_VARS.fd" \
    -drive "if=none,id=validationos,format=qcow2,file=$image" \
    -device ide-hd,drive=validationos,bus=ide.0,bootindex=1 \
    -nic none -vga std -display none \
    -monitor "unix:$monitor,server=on,wait=off" \
    -pidfile "$pidfile" -daemonize
qemu_pid="$(<"$pidfile")"

printf 'booting pristine ValidationOS Win4 so Windows can enable test signing...\n'
for _ in $(seq 1 48); do
    sleep 10
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        break
    fi
    {
        printf 'sendkey w\n'
        sleep 0.3
        printf 'sendkey ret\n'
    } | socat - "UNIX-CONNECT:$monitor" >/dev/null 2>&1 || true
done

if kill -0 "$qemu_pid" 2>/dev/null; then
    printf 'ValidationOS did not finish the preparation command within 8 minutes.\n' >&2
    exit 8
fi
qemu_pid=""

attach_image
mount_validationos
marker="$mount_root/Windows/System32/win0doom-bcd-ready.txt"
if [[ ! -f "$marker" ]] || [[ "$(tr -d '\r\n' < "$marker")" != ready ]]; then
    printf 'Windows preparation failed: %s\n' "$(tr -d '\r\n' < "$marker" 2>/dev/null || printf 'no marker')" >&2
    if [[ -f "$mount_root/Windows/System32/win0doom-bcdboot.log" ]]; then
        sed 's/\r$//' "$mount_root/Windows/System32/win0doom-bcdboot.log" >&2
    fi
    exit 9
fi
generated_store="$mount_root/Windows/Temp/BCD.win0doom"
installed_store="$mount_efi/EFI/Microsoft/Boot/BCD"
if [[ ! -f "$generated_store" || ! -f "$installed_store" ]]; then
    printf 'Windows did not leave a generated BCD store to install.\n' >&2
    exit 10
fi
hivexregedit --export --prefix 'HKEY_LOCAL_MACHINE\BCD00000000' \
    "$generated_store" \
    '\Objects\{f0e79e13-a4e6-11f1-a0a0-806e6f6e6963}\Elements\21000001' | \
    sed 's/\\21000001]/\\21000026]/' > "$bcd_patch"
printf '\n[HKEY_LOCAL_MACHINE\\BCD00000000\\Objects\\{f0e79e13-a4e6-11f1-a0a0-806e6f6e6963}\\Elements\\26000006]\n"Element"=hex(3):00\n' \
    >> "$bcd_patch"
hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\BCD00000000' --encoding ASCII \
    "$generated_store" "$bcd_patch"
cp -p -- "$installed_store" "$mount_efi/EFI/Microsoft/Boot/BCD.pre-win0doom"
cp -p -- "$generated_store" "$installed_store"
rm -f -- "$mount_efi/EFI/Microsoft/Boot/BCD.LOG" \
    "$mount_efi/EFI/Microsoft/Boot/BCD.LOG1" \
    "$mount_efi/EFI/Microsoft/Boot/BCD.LOG2"
target_system="$mount_root/Windows/System32/config/SYSTEM.win0doom-target"
if [[ -f "$target_system" ]]; then
    mv -- "$mount_root/Windows/System32/config/SYSTEM" \
        "$mount_root/Windows/System32/config/SYSTEM.win0doom-win4-used"
    mv -- "$target_system" "$mount_root/Windows/System32/config/SYSTEM"
    for transaction_log in SYSTEM.LOG SYSTEM.LOG1 SYSTEM.LOG2; do
        if [[ -f "$mount_root/Windows/System32/config/$transaction_log" ]]; then
            mv -- "$mount_root/Windows/System32/config/$transaction_log" \
                "$mount_root/Windows/System32/config/$transaction_log.win0doom-win4-used"
        fi
    done
fi
rm -f -- "$mount_root/Windows/System32/w.cmd" "$marker" \
    "$mount_root/Windows/System32/win0doom-runlevel.reg" \
    "$mount_root/Windows/System32/win0doom-driver.reg" \
    "$mount_root/Windows/System32/win0doom-runlevel-target.reg" \
    "$mount_root/Windows/System32/win0doom-driver-target.reg" \
    "$mount_root/Windows/System32/win0doom-bcdboot.log" "$generated_store" \
    "$generated_store.LOG" "$generated_store.LOG1" "$generated_store.LOG2"
detach_image
printf 'Windows prepared the BCD, runlevel 0, and Win0 Doom driver successfully.\n'
