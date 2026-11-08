#!/usr/bin/env bash
# win0-doom headless audio + framebuffer capture harness.
#
# Reproducer (run on the proxmox host, never against the pristine image):
#
#   cp --reflink=auto /var/lib/vz/win0doom/win0-doom.qcow2 \
#       /root/audioharness/run.qcow2
#   tools/audio-capture.sh \
#       --image /root/audioharness/run.qcow2 \
#       --artifacts /root/audioharness/run-$(date +%Y%m%d-%H%M%S) \
#       --seconds 150
#
# Then inspect:
#
#   tools/audio-report.py \
#       --wav DIR/capture.wav --out-dir DIR/report
#
# What it does: boots the qcow2 copy headless under OVMF, takes boot.ppm at
# 60 s plus a screendump every 10 s into frames/, runs the key scenario with
# one HMP `sendkey` per socat invocation (~300 ms apart, the only reliable
# injection method for ccs at runlevel 0), takes final.ppm after the last
# key, records the guest's sb16 output through qemu's wav audiodev, shuts
# down with HMP `quit` (never SIGKILL), converts every PPM to PNG with
# python3+PIL, and writes run.json with timings, screendump color counts,
# and WAV metadata. qemu 11.1.3's wav backend can leave the RIFF/data sizes
# zeroed even on `quit`; tools/audio-report.py derives the payload length
# from the file and validates the fmt chunk. After qemu exits the script
# also tries, best effort and read-only, to attach the image with qemu-nbd,
# mount the CORESYSTEM/ValidationOS NTFS volume with ntfs-3g -o ro, and copy
# C:\Windows\win0doom-audio.log to driver-audio.log; the path (or null) and
# failure note land in run.json.
#
# The image must be a writable copy. The pristine
# /var/lib/vz/win0doom/win0-doom.qcow2 is refused by name and by realpath.
#
# --seconds is the requested capture length (default 150). The run can last a
# few seconds longer when the scenario is still being injected; final.ppm is
# always taken after the last key.
#
# Scenario file syntax (default: tools/audio-scenario.txt):
#   # comment
#   esc                 bare HMP key name -> sendkey esc
#   key spc             explicit key form
#   sleep 1.5           pause
#   screendump demo     capture demo.ppm in the artifacts directory
#
# Exit status: 0 when the run produced run.json and a boot screendump even if
# the WAV is absent/silent (that is evidence, not failure); nonzero when the
# image is refused, qemu fails, the monitor never answers, or a screendump is
# missing.

set -euo pipefail

BOOT_WAIT=60
FRAME_INTERVAL=10
KEY_DELAY=0.3
SETTLE=1.5
PRISTINE=/var/lib/vz/win0doom/win0-doom.qcow2

usage() {
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
    exit "${1:-0}"
}

die() {
    printf 'audio-capture: %s\n' "$*" >&2
    exit 1
}

image=
artifacts=
seconds=150
scenario=
audio_enabled=1

while [ $# -gt 0 ]; do
    case "$1" in
        --image)
            [ $# -ge 2 ] || die '--image requires a value'
            image=$2
            shift 2
            ;;
        --image=*)
            image=${1#*=}
            shift
            ;;
        --artifacts)
            [ $# -ge 2 ] || die '--artifacts requires a value'
            artifacts=$2
            shift 2
            ;;
        --artifacts=*)
            artifacts=${1#*=}
            shift
            ;;
        --seconds)
            [ $# -ge 2 ] || die '--seconds requires a value'
            seconds=$2
            shift 2
            ;;
        --seconds=*)
            seconds=${1#*=}
            shift
            ;;
        --scenario)
            [ $# -ge 2 ] || die '--scenario requires a value'
            scenario=$2
            shift 2
            ;;
        --scenario=*)
            scenario=${1#*=}
            shift
            ;;
        --no-audio)
            audio_enabled=0
            shift
            ;;
        -h|--help)
            usage 0
            ;;
        *)
            usage 2
            ;;
    esac
done

[ -n "$image" ] || die 'missing --image <qcow2>'
[ -n "$artifacts" ] || die 'missing --artifacts <dir>'
[[ "$seconds" =~ ^[0-9]+$ ]] || die "--seconds must be an integer, got '$seconds'"
[ "$seconds" -ge 75 ] || die '--seconds must be at least 75 (boot.ppm is taken at 60 s)'

[ -f "$image" ] || die "image not found: $image"
command -v qemu-system-x86_64 >/dev/null || die 'qemu-system-x86_64 not found'
command -v qemu-img >/dev/null || die 'qemu-img not found'
command -v socat >/dev/null || die 'socat not found'
command -v timeout >/dev/null || die 'timeout not found'
command -v python3 >/dev/null || die 'python3 not found'
python3 -c 'import PIL' >/dev/null 2>&1 \
    || die 'python3 Pillow is required to convert screendumps to PNG'

image_real=$(readlink -f -- "$image")
pristine_real=$(readlink -f -- "$PRISTINE")

file_id() {
    stat -c '%d:%i' -- "$1" 2>/dev/null && return 0
    stat -f '%d:%i' -- "$1" 2>/dev/null && return 0
    printf '%s\n' "$1"
}

if [ "$image_real" = "$pristine_real" ] || [ "$image" = "$PRISTINE" ]; then
    die "refusing the pristine image: $PRISTINE (copy it first, e.g. cp --reflink=auto)"
fi
image_id=$(file_id "$image_real")
pristine_id=$(file_id "$pristine_real")
if [ "$image_id" = "$pristine_id" ] && [ "$image_id" != "$image_real" ]; then
    die "refusing a hardlink to the pristine image: $PRISTINE"
fi

image_format=$(qemu-img info --output=json -- "$image" | python3 -c \
    'import json,sys; print(json.load(sys.stdin).get("format",""))')
[ "$image_format" = qcow2 ] || die "expected a qcow2 image, got '$image_format'"

ovmf_code=/usr/share/OVMF/OVMF_CODE_4M.fd
ovmf_vars_src=/usr/share/OVMF/OVMF_VARS_4M.fd
[ -f "$ovmf_code" ] || die "missing $ovmf_code"
[ -f "$ovmf_vars_src" ] || die "missing $ovmf_vars_src"

mkdir -p -- "$artifacts"
artifacts=$(readlink -f -- "$artifacts")
mkdir -p -- "$artifacts/frames"

# Remove only this tool's previous outputs; never touch unrelated files.
rm -f -- "$artifacts/capture.wav" "$artifacts/boot.ppm" "$artifacts/boot.png" \
    "$artifacts/final.ppm" "$artifacts/final.png" "$artifacts/run.json" \
    "$artifacts/screendumps.jsonl" "$artifacts/events.jsonl" \
    "$artifacts/qemu.stderr.log" "$artifacts/qemu.stdout.log" \
    "$artifacts/hmp.log" "$artifacts/qemu-args.txt" \
    "$artifacts/OVMF_VARS_4M.fd" "$artifacts/driver-audio.log" \
    "$artifacts/driver-log-extract.log" \
    "$artifacts/.driver-audio-log-path" "$artifacts/.driver-audio-log-note"
rm -f -- "$artifacts"/frames/frame-* "$artifacts"/window-*.ppm \
    "$artifacts"/window-*.png

cp -- "$ovmf_vars_src" "$artifacts/OVMF_VARS_4M.fd"
ovmf_vars=$artifacts/OVMF_VARS_4M.fd

if [ -z "$scenario" ]; then
    scenario=$(dirname -- "$(readlink -f -- "$0")")/audio-scenario.txt
fi
[ -f "$scenario" ] || die "scenario file not found: $scenario"
scenario=$(readlink -f -- "$scenario")

# Validate the scenario before spending a boot on it.
while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    line=${line#"${line%%[![:space:]]*}"}
    line=${line%"${line##*[![:space:]]}"}
    [ -z "$line" ] && continue
    [[ "$line" == \#* ]] && continue
    read -r verb arg extra <<<"$line"
    case "$verb" in
        sleep)
            [ -n "$arg" ] && [ -z "${extra:-}" ] || die "bad scenario line: $line"
            [[ "$arg" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "bad sleep: $line"
            ;;
        key)
            [ -n "$arg" ] && [ -z "${extra:-}" ] || die "bad scenario line: $line"
            [[ "$arg" =~ ^[A-Za-z0-9_.-]+$ ]] || die "bad key name: $line"
            ;;
        screendump)
            [ -n "$arg" ] && [ -z "${extra:-}" ] || die "bad scenario line: $line"
            [[ "$arg" =~ ^[A-Za-z0-9._/-]+$ && "$arg" != /* && "$arg" != *..* ]] \
                || die "bad screendump name: $line"
            ;;
        *)
            [ -z "${arg:-}" ] && [ -z "${extra:-}" ] || die "unknown scenario directive: $line"
            [[ "$verb" =~ ^[A-Za-z0-9_.-]+$ ]] || die "bad key name: $line"
            ;;
    esac
done < "$scenario"

monitor=${TMPDIR:-/tmp}/win0doom-audio-$$.sock
rm -f -- "$monitor"
screendumps=$artifacts/screendumps.jsonl
events=$artifacts/events.jsonl
: > "$screendumps"
: > "$events"
qemu_pid=
qemu_rc=
start_ts=
ended_ts=

now_s() {
    local t=${EPOCHREALTIME:-$(date +%s.%N)}
    printf '%s' "${t/,/.}"
}

elapsed_s() {
    awk -v a="$(now_s)" -v b="$start_ts" 'BEGIN { printf "%.3f", a - b }'
}

sleep_until() {
    local target=$1
    while :; do
        local e
        e=$(elapsed_s)
        awk -v e="$e" -v t="$target" 'BEGIN { exit !(e >= t) }' && break
        sleep 0.2
    done
}

hmp() {
    local command=$1 timeout=${2:-3} output status=0
    output=$(printf '%s\n' "$command" | socat -t "$timeout" - \
        "UNIX-CONNECT:$monitor" 2>&1) || status=$?
    printf '%s\n' "$output" >>"$artifacts/hmp.log"
    return "$status"
}

log_event() {
    local action=$1 value=$2 ok=${3:-1} t
    t=$(elapsed_s)
    python3 - "$events" "$action" "$t" "$value" "$ok" <<'PY'
import json, sys
path, action, t_s, value, ok = sys.argv[1:6]
okay = ok not in ("0", "false", "False", "")
with open(path, "a", encoding="utf-8") as handle:
    handle.write(json.dumps({
        "t_s": float(t_s),
        "action": action,
        "value": value,
        "ok": okay,
    }) + "\n")
PY
}

capture_screendump() {
    local name=$1 phase=$2 t ppm
    t=$(elapsed_s)
    case "$name" in
        /*|*..*) die "screendump name must be a safe relative path: $name" ;;
    esac
    [[ "$name" =~ ^[A-Za-z0-9._/-]+$ ]] || die "bad screendump name: $name"
    ppm=$artifacts/$name.ppm
    mkdir -p -- "$(dirname -- "$ppm")"
    rm -f -- "$ppm"
    if ! hmp "screendump $ppm" 5; then
        printf 'audio-capture: warning: screendump monitor command failed: %s\n' "$ppm" >&2
    fi
    local i size last=-1 magic
    for i in $(seq 1 100); do
        size=$(stat -c %s -- "$ppm" 2>/dev/null || \
            stat -f %z -- "$ppm" 2>/dev/null || printf 0)
        if [ "$size" -gt 0 ] && [ "$size" -eq "$last" ]; then
            break
        fi
        last=$size
        sleep 0.1
    done
    [ -s "$ppm" ] || die "screendump did not produce $ppm"
    magic=$(head -c 2 -- "$ppm" 2>/dev/null || true)
    [ "$magic" = "P6" ] || die "screendump did not produce a P6 PPM: $ppm"
    python3 - "$screendumps" "$name" "$phase" "$t" "$ppm" <<'PY'
import json, sys
path, name, phase, t_s, ppm = sys.argv[1:6]
with open(path, "a", encoding="utf-8") as handle:
    handle.write(json.dumps({
        "name": name,
        "phase": phase,
        "t_s": float(t_s),
        "ppm": ppm,
    }) + "\n")
PY
}

run_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo -n "$@"
    fi
}

extract_driver_log() {
    # Best effort: read-only qemu-nbd + ntfs-3g, never write the image, never
    # fail the run.  Extract C:\Windows\win0doom-audio.log when present.
    local result="$artifacts/.driver-audio-log-path"
    local note="$artifacts/.driver-audio-log-note"
    rm -f -- "$result" "$note"
    (
        set +e
        exec >>"$artifacts/driver-log-extract.log" 2>&1
        local nbd="" mount_dir="" reason="not attempted" driver_log=""
        local server_pid="" size="" partition="" candidate tool i

        extract_cleanup() {
            if [ -n "$mount_dir" ]; then
                run_root umount "$mount_dir" >/dev/null 2>&1
            fi
            if [ -n "$nbd" ]; then
                server_pid=""
                if [ -r "/sys/class/block/${nbd##*/}/pid" ]; then
                    server_pid=$(<"/sys/class/block/${nbd##*/}/pid")
                fi
                run_root qemu-nbd --disconnect "$nbd" >/dev/null 2>&1
                if [ -n "$server_pid" ]; then
                    for i in $(seq 1 80); do
                        kill -0 "$server_pid" 2>/dev/null || break
                        sleep 0.25
                    done
                fi
                for i in $(seq 1 80); do
                    qemu-io -f qcow2 -c quit "$image" >/dev/null 2>&1 && break
                    sleep 0.25
                done
            fi
            if [ -n "$mount_dir" ]; then
                rmdir "$mount_dir" >/dev/null 2>&1
            fi
            if [ -n "$driver_log" ]; then
                printf '%s\n' "$driver_log" > "$result"
            fi
            printf '%s\n' "$reason" > "$note"
        }
        trap extract_cleanup EXIT

        for tool in qemu-nbd qemu-io blockdev partprobe lsblk ntfs-3g; do
            command -v "$tool" >/dev/null 2>&1 || {
                reason="missing $tool"
                exit 0
            }
        done
        if command -v modprobe >/dev/null 2>&1; then
            run_root modprobe nbd max_part=16 >/dev/null 2>&1
        fi
        for candidate in /sys/class/block/nbd*; do
            [ -e "$candidate/size" ] || continue
            if [ "$(<"$candidate/size")" = 0 ]; then
                nbd="/dev/${candidate##*/}"
                break
            fi
        done
        if [ -z "$nbd" ]; then
            reason="no unused nbd device"
            exit 0
        fi
        mount_dir=$(mktemp -d /tmp/win0doom-audio-mount.XXXXXX) || {
            reason="mktemp failed"
            exit 0
        }
        if ! run_root qemu-nbd --read-only --format=qcow2 --connect="$nbd" "$image"; then
            reason="qemu-nbd --read-only failed"
            exit 0
        fi
        size=0
        for i in $(seq 1 80); do
            size=$(run_root blockdev --getsize64 "$nbd" 2>/dev/null)
            [ -n "$size" ] && [ "$size" -gt 0 ] && break
            sleep 0.25
        done
        if [ -z "$size" ] || [ "$size" -eq 0 ]; then
            reason="nbd device never became readable"
            exit 0
        fi
        run_root partprobe "$nbd" >/dev/null 2>&1
        if command -v udevadm >/dev/null 2>&1; then
            udevadm settle >/dev/null 2>&1 || sleep 0.5
        else
            sleep 0.5
        fi
        partition=$(lsblk -nrpo NAME,LABEL "$nbd" 2>/dev/null | \
            awk '$2 == "CORESYSTEM" || $2 == "ValidationOS" { print $1; exit }')
        if [ -z "$partition" ]; then
            reason="CORESYSTEM/ValidationOS partition not found"
            exit 0
        fi
        if ! run_root ntfs-3g -o "ro,uid=$(id -u),gid=$(id -g)" \
            "$partition" "$mount_dir"; then
            reason="ntfs-3g read-only mount failed"
            exit 0
        fi
        if [ -f "$mount_dir/Windows/win0doom-audio.log" ]; then
            if cp -- "$mount_dir/Windows/win0doom-audio.log" \
                "$artifacts/driver-audio.log"; then
                driver_log="$artifacts/driver-audio.log"
                reason="extracted Windows/win0doom-audio.log"
            else
                reason="copy of Windows/win0doom-audio.log failed"
            fi
        else
            reason="Windows/win0doom-audio.log not present"
        fi
        exit 0
    )
    driver_audio_log=""
    driver_log_note="no note"
    if [ -s "$result" ]; then
        driver_audio_log=$(<"$result")
    fi
    if [ -s "$note" ]; then
        driver_log_note=$(<"$note")
    fi
    rm -f -- "$result" "$note"
}

cleanup_qemu() {
    if [ -n "${qemu_pid:-}" ] && kill -0 "$qemu_pid" 2>/dev/null; then
        hmp quit 5 || true
        local i
        for i in $(seq 1 150); do
            kill -0 "$qemu_pid" 2>/dev/null || break
            sleep 0.1
        done
    fi
    if [ -n "${qemu_pid:-}" ] && kill -0 "$qemu_pid" 2>/dev/null; then
        kill -TERM "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    qemu_pid=
    rm -f -- "$monitor"
}

on_signal() {
    cleanup_qemu
    exit 130
}

trap cleanup_qemu EXIT
trap on_signal INT TERM

qemu_args=(
    qemu-system-x86_64
    -name win0doom-audio-capture
    -machine q35,accel=kvm
    -cpu host
    -smp 4
    -m 8G
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code"
    -drive "if=pflash,format=raw,file=$ovmf_vars"
    -drive "if=none,id=system_disk,format=qcow2,cache=writeback,discard=unmap,file=$image"
    -device "ide-hd,drive=system_disk,bus=ide.0,bootindex=1"
    -vga std
    -display none
    -monitor "unix:$monitor,server=on,wait=off"
    -nic none
    -global ICH9-LPC.disable_s3=1
)
if [ "$audio_enabled" -eq 1 ]; then
    qemu_args+=(
        -device sb16,audiodev=snd0
        -audiodev "wav,id=snd0,path=$artifacts/capture.wav"
    )
fi

printf '%s\n' "${qemu_args[@]}" > "$artifacts/qemu-args.txt"

start_ts=$(now_s)
start_iso=$(date -u +%Y-%m-%dT%H:%M:%SZ)
hard_timeout=$((seconds + 120))
printf 'audio-capture: booting %s (total %s s, audio=%s)\n' \
    "$image" "$seconds" "$([ "$audio_enabled" -eq 1 ] && printf yes || printf no)"

set +e
timeout --signal=TERM "$hard_timeout" "${qemu_args[@]}" \
    </dev/null >"$artifacts/qemu.stdout.log" 2>"$artifacts/qemu.stderr.log" &
qemu_pid=$!
set -e

ready=0
for _ in $(seq 1 150); do
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        die "qemu exited during startup (see $artifacts/qemu.stderr.log)"
    fi
    if [ -S "$monitor" ]; then
        if printf 'info status\n' | socat -t 2 - "UNIX-CONNECT:$monitor" 2>/dev/null \
            | grep -q 'VM status: running'; then
            ready=1
            break
        fi
    fi
    sleep 0.1
done
[ "$ready" -eq 1 ] || die "monitor did not become ready ($monitor)"

# Boot phase: frames every 10 s, then boot.ppm at 60 s.
for t in $(seq "$FRAME_INTERVAL" "$FRAME_INTERVAL" $((BOOT_WAIT - FRAME_INTERVAL))); do
    sleep_until "$t"
    capture_screendump "frames/frame-$(printf '%03d' "$t")" boot
done
sleep_until "$BOOT_WAIT"
capture_screendump boot boot
printf 'audio-capture: boot.ppm at %s s; driving scenario %s\n' \
    "$(elapsed_s)" "$(basename -- "$scenario")"

# Scenario phase: one sendkey per socat, ~300 ms apart.
while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    line=${line#"${line%%[![:space:]]*}"}
    line=${line%"${line##*[![:space:]]}"}
    [ -z "$line" ] && continue
    [[ "$line" == \#* ]] && continue
    read -r verb arg extra <<<"$line"
    case "$verb" in
        sleep)
            log_event sleep "$arg"
            sleep "$arg"
            ;;
        key)
            key_ok=1
            hmp "sendkey $arg" 3 || key_ok=0
            log_event key "$arg" "$key_ok"
            sleep "$KEY_DELAY"
            ;;
        screendump)
            capture_screendump "$arg" scenario
            log_event screendump "$arg" 1
            ;;
        *)
            key_ok=1
            hmp "sendkey $verb" 3 || key_ok=0
            log_event key "$verb" "$key_ok"
            sleep "$KEY_DELAY"
            ;;
    esac
done < "$scenario"

sleep "$SETTLE"
capture_screendump final final
printf 'audio-capture: final.ppm at %s s\n' "$(elapsed_s)"

sleep_until "$seconds"

# Clean shutdown: HMP quit so qemu exits normally. TERM is only a last
# resort inside cleanup_qemu(), never kill -9. The wav header may still be
# left with zero sizes; audio-report.py does not trust those fields.
set +e
hmp quit 5
for _ in $(seq 1 200); do
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$qemu_pid" 2>/dev/null; then
    printf 'audio-capture: warning: qemu did not exit after HMP quit; sending SIGTERM\n' >&2
    kill -TERM "$qemu_pid" 2>/dev/null || true
    for _ in $(seq 1 100); do
        kill -0 "$qemu_pid" 2>/dev/null || break
        sleep 0.1
    done
fi
wait "$qemu_pid"
qemu_rc=$?
qemu_pid=
set -e

ended_ts=$(elapsed_s)
end_iso=$(date -u +%Y-%m-%dT%H:%M:%SZ)

if [ "$qemu_rc" -ne 0 ]; then
    printf 'audio-capture: warning: qemu/timeout wrapper exited with status %s\n' \
        "$qemu_rc" >&2
fi

driver_audio_log=
driver_log_note=
extract_driver_log
if [ -n "$driver_audio_log" ]; then
    printf 'audio-capture: extracted driver audio log: %s\n' "$driver_audio_log"
else
    printf 'audio-capture: driver-audio.log not extracted: %s\n' \
        "$driver_log_note" >&2
fi

python3 - \
    --jsonl "$screendumps" \
    --events "$events" \
    --out "$artifacts/run.json" \
    --image "$image" \
    --artifacts "$artifacts" \
    --scenario "$scenario" \
    --seconds "$seconds" \
    --audio-enabled "$audio_enabled" \
    --capture "$artifacts/capture.wav" \
    --qemu-rc "$qemu_rc" \
    --qemu-args "$artifacts/qemu-args.txt" \
    --started "$start_iso" \
    --ended "$end_iso" \
    --elapsed "$ended_ts" \
    --driver-audio-log "${driver_audio_log:-}" \
    --driver-log-note "${driver_log_note:-}" <<'PY'
import argparse
import json
import os
import wave
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("--jsonl", required=True)
parser.add_argument("--events", required=True)
parser.add_argument("--out", required=True)
parser.add_argument("--image", required=True)
parser.add_argument("--artifacts", required=True)
parser.add_argument("--scenario", required=True)
parser.add_argument("--seconds", type=int, required=True)
parser.add_argument("--audio-enabled", type=int, required=True)
parser.add_argument("--capture", required=True)
parser.add_argument("--qemu-rc", type=int, required=True)
parser.add_argument("--qemu-args", required=True)
parser.add_argument("--started", required=True)
parser.add_argument("--ended", required=True)
parser.add_argument("--elapsed", type=float, required=True)
parser.add_argument("--driver-audio-log", default="")
parser.add_argument("--driver-log-note", default="")
args = parser.parse_args()

screendumps = []
with open(args.jsonl, encoding="utf-8") as handle:
    for line in handle:
        line = line.strip()
        if not line:
            continue
        entry = json.loads(line)
        entry["png"] = os.path.splitext(entry["ppm"])[0] + ".png"
        if not os.path.exists(entry["ppm"]):
            entry["error"] = "missing ppm"
            entry["distinct_colors"] = None
            screendumps.append(entry)
            continue
        try:
            image = Image.open(entry["ppm"]).convert("RGB")
            image.save(entry["png"])
            colors = image.getcolors(1 << 24)
            entry["distinct_colors"] = len(colors) if colors is not None else None
            entry["size"] = list(image.size)
        except Exception as error:  # noqa: BLE001 - keep evidence, never abort
            entry["error"] = f"cannot convert PPM: {error}"
            entry["distinct_colors"] = None
        screendumps.append(entry)

with open(args.events, encoding="utf-8") as handle:
    events = [json.loads(line) for line in handle if line.strip()]

with open(args.qemu_args, encoding="utf-8") as handle:
    qemu_args = [line.rstrip("\n") for line in handle if line.strip()]

capture = args.capture
capture_exists = os.path.isfile(capture)
capture_bytes = os.path.getsize(capture) if capture_exists else None
wav = {"exists": capture_exists, "bytes": capture_bytes}
if capture_exists and capture_bytes and capture_bytes >= 44:
    try:
        with wave.open(capture, "rb") as handle:
            wav.update({
                "readable": True,
                "rate_hz": handle.getframerate(),
                "channels": handle.getnchannels(),
                "sample_width_bits": handle.getsampwidth() * 8,
                "frames": handle.getnframes(),
                "duration_s": round(handle.getnframes() / handle.getframerate(), 6),
            })
    except Exception as error:  # noqa: BLE001 - evidence, not a hard failure
        wav.update({
            "readable": False,
            "error": str(error),
            "note": ("qemu's wav audiodev may leave RIFF/data sizes zeroed "
                     "on shutdown; tools/audio-report.py derives the length "
                     "from the file and validates the fmt chunk"),
        })

boot = next((entry for entry in screendumps if entry["name"] == "boot"), None)
final = next((entry for entry in screendumps if entry["name"] == "final"), None)
boot_colors = boot["distinct_colors"] if boot else None
final_colors = final["distinct_colors"] if final else None


def doom_like(colors):
    # Clean boots measured 111-177 distinct colours in Doom; the text
    # console is ~19.
    return bool(colors is not None and colors >= 80)


report = {
    "tool": "tools/audio-capture.sh",
    "version": 1,
    "image": args.image,
    "artifacts": args.artifacts,
    "started_at_utc": args.started,
    "ended_at_utc": args.ended,
    "elapsed_s": args.elapsed,
    "seconds_requested": args.seconds,
    "driver_audio_log": args.driver_audio_log or None,
    "driver_log_note": args.driver_log_note,
    "qemu": {
        "args": qemu_args,
        "exit_code": args.qemu_rc,
        "exit_code_note": ("status of the timeout wrapper; 124 means the "
                           "hard timeout fired"),
        "stderr_log": os.path.join(args.artifacts, "qemu.stderr.log"),
        "stdout_log": os.path.join(args.artifacts, "qemu.stdout.log"),
    },
    "scenario": {
        "path": args.scenario,
        "events": events,
    },
    "audio": {
        "enabled": bool(args.audio_enabled),
        "device": "sb16" if args.audio_enabled else None,
        "audiodev": "wav" if args.audio_enabled else None,
        "capture_wav": capture,
        "capture_exists": capture_exists,
        "capture_bytes": capture_bytes,
        "wav": wav,
    },
    "screendumps": screendumps,
    "evidence": {
        "boot_colors": boot_colors,
        "final_colors": final_colors,
        "doom_running_at_boot": doom_like(boot_colors),
        "doom_running_at_final": doom_like(final_colors),
        "wav_exists": capture_exists,
        "wav_bytes": capture_bytes,
        "wav_may_be_absent_until_the_guest_drives_sb16": True,
        "driver_audio_log": args.driver_audio_log or None,
    },
}

with open(args.out, "w", encoding="utf-8") as handle:
    json.dump(report, handle, indent=2)
    handle.write("\n")

print("audio-capture: run.json")
print(json.dumps(report["evidence"], indent=2))
PY

printf 'audio-capture: artifacts in %s\n' "$artifacts"
ls -l -- "$artifacts" | sed 's/^/  /'
printf 'audio-capture: frames\n'
ls -l -- "$artifacts/frames" | sed 's/^/  /'

if [ "$qemu_rc" -ne 0 ]; then
    exit 2
fi
