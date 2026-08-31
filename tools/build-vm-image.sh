#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s OFFICIAL_VALIDATIONOS.iso GENERATED_RL0.wim OUTPUT.qcow2 DOOM1.WAD\n' "${0##*/}" >&2
}

if (( $# != 4 )); then
    usage
    exit 2
fi

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
iso="$(realpath -e -- "$1")"
generated_wim="$(realpath -e -- "$2")"
output_parent="$(realpath -e -- "$(dirname -- "$3")")"
output="$output_parent/$(basename -- "$3")"
wad="$(realpath -e -- "$4")"
partial="$output.partial.$$"
iso_mount="$(mktemp -d /tmp/win0doom-iso.XXXXXX)"

if [[ -e "$output" ]]; then
    printf 'output already exists; refusing to overwrite it: %s\n' "$output" >&2
    exit 3
fi

cleanup() {
    sudo umount "$iso_mount" >/dev/null 2>&1 || true
    rmdir "$iso_mount" >/dev/null 2>&1 || true
    if [[ -e "$partial" ]]; then
        mv -- "$partial" "$partial.failed"
        printf 'the incomplete image was preserved at %s.failed\n' "$partial" >&2
    fi
}
trap cleanup EXIT

"$repo_dir/tools/build-release.sh"
sudo mount -o loop,ro "$iso" "$iso_mount"
vhdx="$(find "$iso_mount" -maxdepth 2 -type f -iname 'ValidationOS.vhdx' -print -quit)"
if [[ -z "$vhdx" ]]; then
    printf 'ValidationOS.vhdx was not found in the supplied official ISO.\n' >&2
    exit 4
fi

qemu-img convert -p -f vhdx -O qcow2 -c "$vhdx" "$partial"
qemu-img resize "$partial" 64G
sudo umount "$iso_mount"

"$repo_dir/tools/apply-generated-wim.sh" "$partial" "$generated_wim"
"$repo_dir/tools/prepare-test-signing.sh" "$partial"
"$repo_dir/tools/install-payload.sh" "$partial" "$wad"
qemu-img check "$partial"
mv -- "$partial" "$output"
sha256sum "$output" > "$output.sha256"

printf '\nready-to-boot image: %s\n' "$output"
printf 'sha-256: '
cut -d ' ' -f 1 "$output.sha256"
