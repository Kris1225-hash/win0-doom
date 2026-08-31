#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s TARGET.qcow2 BACKUP.qcow2 --yes\n' "${0##*/}" >&2
}

if (( $# != 3 )) || [[ "$3" != --yes ]]; then
    usage
    exit 2
fi

target="$(realpath -e -- "$1")"
backup="$(realpath -e -- "$2")"
timestamp="$(date -u +%Y%m%d-%H%M%S)"
temporary="$target.restoring.$$"
displaced="$target.pre-rollback-$timestamp"

if [[ "$target" == "$backup" ]]; then
    printf 'target and backup resolve to the same file.\n' >&2
    exit 3
fi
if pgrep -a qemu-system | grep -F -- "$target" >/dev/null; then
    printf 'the target image is attached to a running qemu process; shut it down first.\n' >&2
    exit 4
fi

qemu-img check "$backup"
qemu-img convert -p -f qcow2 -O qcow2 -c "$backup" "$temporary"
qemu-img check "$temporary"
mv -- "$target" "$displaced"
mv -- "$temporary" "$target"

printf 'rollback complete. the displaced image remains recoverable at:\n%s\n' "$displaced"
