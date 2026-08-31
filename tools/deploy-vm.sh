#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s TARGET.qcow2 [DOOM1.WAD]\n' "${0##*/}" >&2
}

if (( $# < 1 || $# > 2 )); then
    usage
    exit 2
fi

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
target="$(realpath -e -- "$1")"
wad="${2:-}"
timestamp="$(date -u +%Y%m%d-%H%M%S)"
backup="${target%.qcow2}.pre-deploy-$timestamp.qcow2"

if pgrep -a qemu-system | grep -F -- "$target" >/dev/null; then
    printf 'the target image is attached to a running qemu process; shut it down first.\n' >&2
    exit 3
fi

"$repo_dir/tools/build-release.sh"
printf 'creating compressed rollback image: %s\n' "$backup"
qemu-img convert -p -f qcow2 -O qcow2 -c "$target" "$backup"
qemu-img check "$backup"
sha256sum "$backup" > "$backup.sha256"

if [[ -n "$wad" ]]; then
    "$repo_dir/tools/install-payload.sh" "$target" "$wad"
else
    "$repo_dir/tools/install-payload.sh" "$target"
fi

printf 'deployment complete. rollback with:\n'
printf '  %q %q %q --yes\n' "$repo_dir/tools/rollback-vm.sh" "$target" "$backup"
