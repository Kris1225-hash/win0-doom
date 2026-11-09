#!/usr/bin/env bash
#
# One command from source to a disposable, payload-deployed ValidationOS disk.
#
#   1. tools/build-release.sh         compile, test-sign, verify (no sudo)
#   2. copy --base to a private qcow2 reflink/copy (base is never modified)
#   3. tools/install-payload.sh       deploy driver, client, wad (needs sudo for
#                                     qemu-nbd + ntfs-3g)
#   4. tools/audio-capture.sh         optional evidence harness, skipped when
#                                     the harness does not exist yet
#
# Every step is echoed as a "+ command" line so the run can be replayed by hand.
# The disposable image is left in place for further boots unless --rm is given;
# the base image is only ever opened for reading.
#
# WIN0DOOM_AUDIO_SELFTEST=1 is exported for the deploy step by default: the
# payload then carries the win0-audio-selftest marker, which makes the native
# client play a deterministic 1 s 440 Hz tone for the capture harness.  Pass
# --no-selftest to deploy without it.
#
# usage: tools/disposable-vm.sh --base IMAGE.qcow2 [options]
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
pristine_image='/var/lib/vz/win0doom/win0-doom.qcow2'

usage() {
    cat >&2 <<'EOF'
usage: disposable-vm.sh --base IMAGE.qcow2 [options]

  --base IMG         source qcow2 (required; also WIN0DOOM_BASE_IMAGE)
  --image OUT        disposable image path
                     (default: dist/win0doom-disposable-<utc>.qcow2)
  --wad PATH         doom wad to install (also WIN0DOOM_WAD)
  --artifacts DIR    evidence directory (default: dist/evidence-<utc>)
  --seconds N        capture length handed to tools/audio-capture.sh
  --allow-pristine   allow the qm-owned pristine image as --base (read only)
  --no-selftest      do not set WIN0DOOM_AUDIO_SELFTEST=1 for the deploy step
  --skip-build       reuse existing release artifacts instead of rebuilding
  --dry-run          print the command trail without running anything
  --rm               delete the disposable image after a successful run
  -h, --help         this text

Options that take a value also accept --name=value.

The build and verification steps never need root.  The deploy step calls
tools/install-payload.sh, which needs sudo for qemu-nbd and ntfs-3g.
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

base=''
image=''
wad="${WIN0DOOM_WAD:-}"
artifacts=''
seconds=''
allow_pristine=0
selftest=1
skip_build=0
dry_run=0
remove_image=0

while (( $# > 0 )); do
    if [[ "$1" == --*=* ]]; then
        set -- "${1%%=*}" "${1#*=}" "${@:2}"
        continue
    fi
    case "$1" in
        --base) [[ $# -ge 2 ]] || die '--base needs a path'; base="$2"; shift 2 ;;
        --image) [[ $# -ge 2 ]] || die '--image needs a path'; image="$2"; shift 2 ;;
        --wad) [[ $# -ge 2 ]] || die '--wad needs a path'; wad="$2"; shift 2 ;;
        --artifacts) [[ $# -ge 2 ]] || die '--artifacts needs a path'; artifacts="$2"; shift 2 ;;
        --seconds) [[ $# -ge 2 ]] || die '--seconds needs a number'; seconds="$2"; shift 2 ;;
        --allow-pristine) allow_pristine=1; shift ;;
        --no-selftest) selftest=0; shift ;;
        --skip-build) skip_build=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        --rm) remove_image=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; die "unknown argument: $1" ;;
    esac
done

[[ -n "$base" ]] || base="${WIN0DOOM_BASE_IMAGE:-}"
[[ -n "$base" ]] || { usage; die 'a base image is required (--base IMAGE.qcow2)'; }
if [[ -n "$seconds" && ! "$seconds" =~ ^[0-9]+$ ]]; then
    die "--seconds must be a whole number of seconds: $seconds"
fi

stamp="$(date -u +%Y%m%d-%H%M%S)"
[[ -n "$image" ]] || image="$repo_dir/dist/win0doom-disposable-$stamp.qcow2"
[[ -n "$artifacts" ]] || artifacts="$repo_dir/dist/evidence-$stamp"

[[ -e "$base" ]] || die "base image does not exist: $base"
[[ -f "$base" ]] || die "base image is not a regular file: $base"
base="$(realpath -e -- "$base")"
image="$(realpath -m -- "$image")"
[[ "$image" != "$base" ]] || die 'the disposable image would overwrite the base image'
[[ ! -e "$image" ]] || die "disposable image already exists: $image"

pristine_real="$(realpath -e -- "$pristine_image" 2>/dev/null || true)"
if [[ -z "$pristine_real" ]]; then
    pristine_real="$pristine_image"
fi
if (( ! allow_pristine )); then
    [[ "$base" != "$pristine_real" ]] ||
        die "$base is the pristine qm image; pass --allow-pristine to copy it (it is only read)"
    if [[ -e "$pristine_real" &&
          "$(stat -c '%d:%i' -- "$base")" == "$(stat -c '%d:%i' -- "$pristine_real")" ]]; then
        die "$base is a hardlink to the pristine qm image; pass --allow-pristine to copy it"
    fi
fi
if [[ "$base" == /var/lib/vz/* ]]; then
    printf 'note: %s is outside the disposable scratch area; it is opened read-only\n' "$base" >&2
fi

if [[ "$base" != *.qcow2 ]]; then
    printf 'note: base image does not end in .qcow2: %s\n' "$base" >&2
fi

command -v qemu-img >/dev/null || die 'qemu-img is required'
command -v cp >/dev/null || die 'cp is required'

base_format="$(qemu-img info -- "$base" | sed -n 's/^file format: *//p' | head -1)"
[[ "$base_format" == 'qcow2' ]] ||
    die "base image format is ${base_format:-unknown}, expected qcow2"

base_in_use() {
    local comm pid fd target name
    if [[ ! -d /proc/self ]]; then
        printf 'warning: /proc is unavailable; cannot check whether %s is in use\n' "$base" >&2
        return 1
    fi
    for comm in /proc/[0-9]*/comm; do
        read -r name <"$comm" 2>/dev/null || continue
        [[ "$name" == qemu-system* ]] || continue
        pid="${comm#/proc/}"
        pid="${pid%/comm}"
        if tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -F -- "$base" >/dev/null; then
            return 0
        fi
        for fd in /proc/"$pid"/fd/*; do
            target="$(readlink -- "$fd" 2>/dev/null || true)"
            if [[ -n "$target" && "$target" == "$base" ]]; then
                return 0
            fi
        done
    done
    return 1
}

if base_in_use; then
    die "$base is attached to a running qemu process; shut that guest down first"
fi

if [[ -n "$wad" ]]; then
    [[ -f "$wad" ]] || die "wad does not exist: $wad"
    wad="$(realpath -e -- "$wad")"
fi

if (( ! dry_run )); then
    backing="$(qemu-img info -- "$base" | sed -n 's/^backing file: *//p' | head -1)"
    if [[ -n "$backing" ]]; then
        printf 'warning: %s is a qcow2 overlay backed by %s; the copy keeps that dependency\n' \
            "$base" "$backing" >&2
    fi
fi

if (( ! dry_run )) && command -v df >/dev/null; then
    base_bytes="$(stat -c %s -- "$base")"
    probe_dir="$(dirname -- "$image")"
    while [[ ! -d "$probe_dir" && "$probe_dir" != '/' ]]; do
        probe_dir="$(dirname -- "$probe_dir")"
    done
    free_bytes="$(df -B1 --output=avail -- "$probe_dir" 2>/dev/null | tail -1 || true)"
    if [[ -n "$free_bytes" && "$free_bytes" =~ ^[0-9]+$ ]] && (( free_bytes < base_bytes )); then
        printf 'note: %s bytes free, base is %s bytes; the copy may not fit\n' \
            "$free_bytes" "$base_bytes" >&2
    fi
fi

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    if (( dry_run )); then
        return 0
    fi
    "$@"
}

image_created=0
on_exit() {
    local status=$?
    if (( status != 0 && image_created )); then
        printf '\nfailed; disposable image kept for inspection: %s\n' "$image" >&2
    fi
}
trap on_exit EXIT

signed_driver="$repo_dir/driver/build/win0doom-display-signed.sys"
native_exe="$repo_dir/native/build/win0doom.exe"
harness="$repo_dir/tools/audio-capture.sh"

printf 'disposable-vm: base=%s\n' "$base"
printf 'disposable-vm: image=%s\n' "$image"
printf 'disposable-vm: artifacts=%s\n' "$artifacts"
if (( dry_run )); then
    printf 'disposable-vm: dry run, nothing is executed\n'
fi

printf '\n== build, sign, verify ==\n'
if (( skip_build )); then
    printf 'skip: --skip-build, reusing %s and %s\n' "$signed_driver" "$native_exe"
else
    run "$repo_dir/tools/build-release.sh"
fi

if (( ! dry_run )); then
    [[ -f "$signed_driver" ]] || die "missing signed driver: $signed_driver"
    [[ -f "$native_exe" ]] || die "missing native client: $native_exe"
fi

installer="$repo_dir/tools/install-payload.sh"
[[ -f "$installer" ]] || die "missing deploy helper: $installer"

printf '\n== disposable image ==\n'
run mkdir -p -- "$(dirname -- "$image")"
copy_image() {
    printf '+'
    printf ' %q' cp --reflink=auto --sparse=always -- "$base" "$image"
    printf '\n'
    if (( dry_run )); then
        return 0
    fi
    if ! cp --reflink=auto --sparse=always -- "$base" "$image"; then
        printf 'error: copying %s to %s failed; free space now:\n' "$base" "$image" >&2
        df -h -- "${probe_dir:-$(dirname -- "$image")}" >&2 || true
        rm -f -- "$image"
        printf 'partial copy removed: %s\n' "$image" >&2
        return 1
    fi
    image_created=1
}
copy_image
check_image() {
    printf '+'
    printf ' %q' qemu-img check -- "$image"
    printf '\n'
    if (( dry_run )); then
        return 0
    fi
    local status=0
    qemu-img check -- "$image" || status=$?
    case "$status" in
        0) ;;
        3)
            # qemu-img(1): exit 3 = leaked clusters only, image not corrupted.
            # A base that was ever attached to a qemu that crashed or was
            # killed carries them into every copy; that wastes space, it does
            # not lose data.
            printf 'note: %s has leaked clusters (qemu-img check exit 3) but is consistent; continuing\n' \
                "$image" >&2
            ;;
        *)
            printf 'error: %s failed qemu-img check (exit %s); removing the copy\n' "$image" "$status" >&2
            rm -f -- "$image"
            image_created=0
            return 1
            ;;
    esac
}
check_image
if (( ! dry_run )); then
    printf 'disposable image created: %s (%s bytes, base untouched)\n' \
        "$image" "$(stat -c %s -- "$image")"
fi

printf '\n== deploy payload (sudo: nbd + ntfs-3g) ==\n'
deploy_cmd=("$installer" "$image")
if [[ -n "$wad" ]]; then
    deploy_cmd+=("$wad")
else
    printf 'deploy: no --wad given; the base image keeps its existing doom1.wad\n'
fi
if (( selftest )); then
    printf 'deploy: WIN0DOOM_AUDIO_SELFTEST=1 installs the win0-audio-selftest marker\n'
    run env WIN0DOOM_AUDIO_SELFTEST=1 "${deploy_cmd[@]}"
else
    printf 'deploy: --no-selftest, no selftest marker is installed\n'
    run "${deploy_cmd[@]}"
fi

printf '\n== evidence harness ==\n'
if [[ -x "$harness" ]]; then
    harness_cmd=("$harness" --image "$image" --artifacts "$artifacts")
    if [[ -n "$seconds" ]]; then
        harness_cmd+=(--seconds "$seconds")
    fi
    run mkdir -p -- "$artifacts"
    run "${harness_cmd[@]}"
    printf 'evidence artifacts: %s\n' "$artifacts"
else
    printf 'skip: %s does not exist yet; deploy-only run, no audio evidence captured\n' \
        "$harness"
fi

printf '\n== result ==\n'
printf 'disposable image: %s\n' "$image"
printf 'artifacts:        %s\n' "$artifacts"
printf 'base (unchanged): %s\n' "$base"

if (( remove_image )); then
    run rm -f -- "$image"
    if (( dry_run )); then
        printf 'dry run: the disposable image would be removed (--rm)\n'
    else
        printf 'disposable image removed (--rm)\n'
    fi
else
    if (( dry_run )); then
        printf 'dry run: the disposable image would be kept\n'
    else
        printf 'disposable image kept; delete it with: rm -f %q\n' "$image"
    fi
fi

printf '\nreplay (fresh image and artifact paths):\n'
replay=(--base "$base")
if [[ -n "$wad" ]]; then
    replay+=(--wad "$wad")
fi
if [[ -n "$seconds" ]]; then
    replay+=(--seconds "$seconds")
fi
if (( allow_pristine )); then
    replay+=(--allow-pristine)
fi
if (( ! selftest )); then
    replay+=(--no-selftest)
fi
if (( skip_build )); then
    replay+=(--skip-build)
fi
if (( remove_image )); then
    replay+=(--rm)
fi
printf '  %q' "$repo_dir/tools/disposable-vm.sh"
printf ' %q' "${replay[@]}"
printf '\n'
