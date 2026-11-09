#!/usr/bin/env bash
#
# Build, test-sign, and verify the Win0 Doom release artifacts.
#
#   1. compile the native client, the input probe, and the display driver
#   2. test-sign win0doom-display.sys with the WDK OEM test certificate
#   3. verify what was produced:
#        - osslsigncode: Authenticode digest + signature of the signed driver
#        - llvm-readobj: win0doom.exe is subsystem:native and imports ntdll.dll
#        - llvm-readobj: win0doom-display.sys is subsystem:native and imports
#          kernel modules only
#        - llvm-nm: if driver/build/win0doom-audio.obj exists, the display
#          object must reference the Win0DoomAudio* entry points, every such
#          reference must be defined by the audio object, and the driver must
#          have been relinked after the audio object appeared
#   4. write driver/build/release.sha256 and driver/build/release-manifest.txt
#
# This script compiles, signs, and inspects files.  It needs no sudo.
#
# Environment (all optional, unchanged from earlier revisions):
#   WINDOWS_SDK_ROOT            extracted SDK/WDK root
#                               (default: <repo>/toolchain/wdk-26100/c)
#   OSSLSIGNCODE                osslsigncode binary (default:
#                               <repo>/toolchain/osslsigncode/build/osslsigncode,
#                               then `osslsigncode` from PATH)
#   WIN0DOOM_SIGN_PFX           signing container
#                               (default: $WINDOWS_SDK_ROOT/tools/certificates/OEM_Test_Cert_2017.pfx)
#   WIN0DOOM_SIGN_INTERMEDIATE  cross certificate, PEM preferred
#                               (default: first existing WDK OEM 2017 intermediate)
#   WIN0DOOM_SIGN_PASSWORD      pfx password (default: empty)
#   WIN0DOOM_SIGN_NAME          signature description text
#                               (default: "Win0 Doom display and keyboard bridge")
#   LLVM_BIN                    directory holding clang-cl/lld-link/llvm-readobj/llvm-nm;
#                               passed to the build scripts as well, so one
#                               toolchain both builds and inspects the release
#                               (default: /usr/lib/llvm/22/bin)
#
# The WDK OEM test root certificate has no CA basic-constraints extension, so
# OpenSSL (and therefore osslsigncode) refuses to complete the certificate
# chain even though Windows test signing accepts it.  The verification below
# requires the Authenticode digest to match the file, the PE checksum to cover
# the signed overlay, an independent PKCS#7 signature check with `openssl
# smime -verify -noverify -content` (OpenSSL's PKCS7_verify aborts at the
# chain before touching the RSA signature; the signed SpcIndirectDataContent
# body is passed explicitly so OpenSSL 3.0-3.4 digest the same bytes as 3.5+),
# and the signer to be the certificate from the
# pfx; a chain failure is tolerated only when OpenSSL fails for that specific
# trust-anchor reason and nothing else.  Any other error, a digest or
# checksum mismatch, a missing signature, or a different signer fails the
# build.  The pfx is a local build input that ships with the extracted SDK:
# this proves the artifact matches that key and was not modified after signing,
# not that a public trust chain vouches for it.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${WINDOWS_SDK_ROOT:-$repo_dir/toolchain/wdk-26100/c}"
llvm_bin="${LLVM_BIN:-/usr/lib/llvm/22/bin}"
description="${WIN0DOOM_SIGN_NAME:-Win0 Doom display and keyboard bridge}"
password="${WIN0DOOM_SIGN_PASSWORD:-}"

pfx="${WIN0DOOM_SIGN_PFX:-$sdk_root/tools/certificates/OEM_Test_Cert_2017.pfx}"

native_exe="$repo_dir/native/build/win0doom.exe"
probe_exe="$repo_dir/native/build/inputprobe.exe"
unsigned_driver="$repo_dir/driver/build/win0doom-display.sys"
signed_driver="$repo_dir/driver/build/win0doom-display-signed.sys"
signed_temporary="$repo_dir/driver/build/.win0doom-display-signed.$$.sys"
display_object="$repo_dir/driver/build/win0doom-display.obj"
audio_object="$repo_dir/driver/build/win0doom-audio.obj"
sha_file="$repo_dir/driver/build/release.sha256"
manifest_file="$repo_dir/driver/build/release-manifest.txt"

say() { printf '%s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    if [[ -e "$sha_file" || -e "$manifest_file" ]]; then
        rm -f -- "$sha_file" "$manifest_file"
        printf 'removed the previous release checksum and manifest; no release was produced\n' >&2
    fi
    exit 1
}

# ---------------------------------------------------------------------------
# tool and certificate resolution
# ---------------------------------------------------------------------------

if [[ -n "${OSSLSIGNCODE:-}" ]]; then
    signer="$OSSLSIGNCODE"
    [[ -x "$signer" ]] || fail "OSSLSIGNCODE is not executable: $signer"
else
    signer="$repo_dir/toolchain/osslsigncode/build/osslsigncode"
    if [[ ! -x "$signer" ]]; then
        signer="$(command -v osslsigncode || true)"
    fi
    [[ -n "$signer" && -x "$signer" ]] ||
        fail 'osslsigncode not found; set OSSLSIGNCODE=/path/to/osslsigncode'
fi

for tool in clang-cl lld-link llvm-readobj llvm-nm; do
    [[ -x "$llvm_bin/$tool" ]] || fail "missing tool: $llvm_bin/$tool"
done
[[ -f "$pfx" ]] || fail "signing container is missing: $pfx"

intermediate=""
if [[ -n "${WIN0DOOM_SIGN_INTERMEDIATE:-}" ]]; then
    intermediate="$(realpath -e -- "$WIN0DOOM_SIGN_INTERMEDIATE" 2>/dev/null || true)"
    [[ -n "$intermediate" && -f "$intermediate" ]] ||
        fail "WIN0DOOM_SIGN_INTERMEDIATE does not exist: $WIN0DOOM_SIGN_INTERMEDIATE"
else
    for candidate in \
        "$repo_dir/toolchain/OEM_Intermediate_Cert_2017.pem" \
        "$sdk_root/tools/certificates/OEM_Intermediate_Cert_2017.pem" \
        "$sdk_root/tools/certificates/OEM_Intermediate_Cert_2017.cer" \
        "$repo_dir/toolchain/OEM_Intermediate_Cert_2017.cer"; do
        if [[ -f "$candidate" ]]; then
            intermediate="$candidate"
            break
        fi
    done
    [[ -n "$intermediate" ]] ||
        fail 'no OEM 2017 intermediate certificate found; set WIN0DOOM_SIGN_INTERMEDIATE'
fi

work_dir="$(mktemp -d /tmp/win0doom-release.XXXXXX)"
cleanup() {
    if [[ -e "$signed_temporary" ]]; then
        mv -f -- "$signed_temporary" "$signed_temporary.failed"
    fi
    rm -rf -- "$work_dir"
}
trap cleanup EXIT

# osslsigncode reads -ac / -CAfile as PEM; convert DER input when needed.
to_pem() {
    local source="$1"
    case "$source" in
        *.pem) cat -- "$source" ;;
        *)
            command -v openssl >/dev/null ||
                fail "openssl is required to convert $source to PEM"
            if openssl x509 -in "$source" -noout 2>/dev/null; then
                openssl x509 -in "$source"
            else
                openssl x509 -inform DER -in "$source" 2>/dev/null ||
                    fail "cannot convert $source to PEM"
            fi
            ;;
    esac
}

cross_pem="$intermediate"
case "$intermediate" in
    *.pem) ;;
    *) cross_pem="$work_dir/intermediate.pem"; to_pem "$intermediate" >"$cross_pem" ;;
esac

if command -v openssl >/dev/null; then
    leaf_issuer_cn="$(openssl pkcs12 -in "$pfx" -nokeys -passin "pass:$password" -nodes 2>/dev/null |
        openssl x509 -noout -issuer 2>/dev/null |
        sed -n 's/.*CN=\(.*\)/\1/p')"
    cross_cn="$(openssl x509 -in "$cross_pem" -noout -subject 2>/dev/null |
        sed -n 's/.*CN=\(.*\)/\1/p')"
    [[ -n "$leaf_issuer_cn" && -n "$cross_cn" ]] ||
        fail "cannot read the $pfx issuer or the cross certificate subject"
    [[ "$leaf_issuer_cn" == "$cross_cn" ]] ||
        fail "cross certificate CN=\"$cross_cn\" does not issue the signing certificate (issuer CN=\"$leaf_issuer_cn\")"
fi

# Assemble the WDK OEM 2017 root + intermediate as a trust bundle.  The chain
# still ends at a root OpenSSL does not accept (it has no CA basic constraints),
# but a complete bundle proves the intermediate really is the expected one.
trust_pem=""
if command -v openssl >/dev/null; then
    for root in \
        "$sdk_root/tools/certificates/OEM_Root_CA_2017.pem" \
        "$sdk_root/tools/certificates/OEM_Root_CA_2017.cer" \
        "$repo_dir/toolchain/OEM_Root_CA_2017.pem" \
        "$repo_dir/toolchain/OEM_Root_CA_2017.cer"; do
        [[ -f "$root" ]] || continue
        trust_pem="$work_dir/test-root-chain.pem"
        to_pem "$root" >"$trust_pem"
        cat -- "$cross_pem" >>"$trust_pem"
        break
    done
fi

say "build-release: sdk=$sdk_root"
say "build-release: signer=$signer"
say "build-release: pfx=$pfx"
say "build-release: intermediate=$intermediate"

# ---------------------------------------------------------------------------
# build + sign
# ---------------------------------------------------------------------------

say ''
say '== build =='
WINDOWS_SDK_ROOT="$sdk_root" LLVM_BIN="$llvm_bin" "$repo_dir/native/build-win0-doom.sh"
WINDOWS_SDK_ROOT="$sdk_root" LLVM_BIN="$llvm_bin" "$repo_dir/native/build-input-probe.sh"
WINDOWS_SDK_ROOT="$sdk_root" LLVM_BIN="$llvm_bin" "$repo_dir/driver/build-driver.sh"

say ''
say '== sign =='
"$signer" sign \
    -pkcs12 "$pfx" \
    -pass "$password" \
    -ac "$cross_pem" \
    -h sha256 \
    -n "$description" \
    -in "$unsigned_driver" \
    -out "$signed_temporary"
mv -f -- "$signed_temporary" "$signed_driver"
say "signed $signed_driver"

# ---------------------------------------------------------------------------
# verification helpers
# ---------------------------------------------------------------------------

file_size() { stat -c %s -- "$1"; }
file_sha256() { sha256sum -- "$1" | awk '{print $1}'; }

pe_subsystem() {
    "$llvm_bin/llvm-readobj" --file-headers "$1" |
        awk '/^ *Subsystem:/ { print $2; exit }'
}

pe_import_list() {
    "$llvm_bin/llvm-readobj" --coff-imports "$1" |
        awk '/^ *Name: / { sub(/^ *Name: */, ""); sub(/[ \t\r]+$/, ""); print }' |
        LC_ALL=C sort -u
}

pe_imports() {
    pe_import_list "$1" | paste -sd, -
}

# ---------------------------------------------------------------------------
# signature verification
# ---------------------------------------------------------------------------

# Authenticode signs the body of SpcIndirectDataContent (content type
# 1.3.6.1.4.1.311.2.1.4): the SEQUENCE contents without their tag and length.
# OpenSSL 3.5+ extracts that body itself, but earlier releases hash an empty
# body for any content type other than "data", so `openssl smime -verify`
# reports "digest failure" for every valid Authenticode signature on
# OpenSSL 3.0-3.4.  Cut the body out of the DER blob and pass it with
# -content; every OpenSSL 3 release then digests the same bytes.
authenticode_content() {
    local blob="$1" out="$2" offset='' header='' length=''
    read -r offset header length < <(
        openssl asn1parse -inform DER -in "$blob" 2>/dev/null |
            sed -n '/:1\.3\.6\.1\.4\.1\.311\.2\.1\.4 *$/,$p' |
            sed -n '/cons: SEQUENCE/{s/^ *\([0-9]*\):d= *[0-9]* *hl=\([0-9]*\) *l= *\([0-9]*\) .*/\1 \2 \3/p;q;}'
    ) || true
    [[ "$offset" =~ ^[0-9]+$ && "$header" =~ ^[0-9]+$ && "$length" =~ ^[0-9]+$ ]] ||
        return 1
    tail -c "+$((offset + header + 1))" -- "$blob" | head -c "$length" >"$out"
    [[ "$(file_size "$out")" == "$length" ]]
}

signature_status='unverified'
verify_signature() {
    local log="$work_dir/osslsigncode-verify.log"
    local rc=0 args current calculated signer_subject signer_cn expected_cn unexpected

    args=(verify -in "$signed_driver")
    if [[ -n "$trust_pem" ]]; then
        args+=(-CAfile "$trust_pem")
    fi

    "$signer" "${args[@]}" >"$log" 2>&1 || rc=$?

    current="$(grep -m1 -oE 'Current message digest[^:]*: *[0-9A-Fa-f]{64}' "$log" |
        grep -oE '[0-9A-Fa-f]{64}' | tr 'A-F' 'a-f' || true)"
    calculated="$(grep -m1 -oE 'Calculated message digest[^:]*: *[0-9A-Fa-f]{64}' "$log" |
        grep -oE '[0-9A-Fa-f]{64}' | tr 'A-F' 'a-f' || true)"

    grep -q 'Signature Index: 0' "$log" ||
        fail "no Authenticode signature in $signed_driver"
    [[ -n "$current" && -n "$calculated" ]] ||
        fail "osslsigncode printed no signature digest for $signed_driver"
    [[ "$current" == "$calculated" ]] ||
        fail "Authenticode digest mismatch: signed=$current file=$calculated"
    if grep -q 'MISMATCH' "$log"; then
        fail "Authenticode digest mismatch reported by osslsigncode"
    fi
    # A correct signature has a matching PE checksum; osslsigncode only prints
    # the comparison and warning when a byte changed after signing, including
    # bytes inside the WIN_CERTIFICATE overlay.
    if grep -qi 'invalid PE checksum' "$log"; then
        fail "PE checksum does not match the signed image; the file or the certificate overlay was modified"
    fi

    # OpenSSL rejects the WDK test root, and PKCS7_verify() aborts at chain
    # verification before it ever checks the RSA signature.  A corrupt
    # signature must not hide behind the tolerated chain error, so verify the
    # extracted PKCS#7 signature separately (signature over the signed
    # attributes and their message digest, chain ignored).  The signed
    # content is passed explicitly: see authenticode_content().
    signature_blob="$work_dir/signature.p7"
    signed_content="$work_dir/signature.content"
    "$signer" extract-signature -in "$signed_driver" -out "$signature_blob" >/dev/null 2>&1 ||
        fail "cannot extract the Authenticode signature from $signed_driver"
    command -v openssl >/dev/null ||
        fail 'openssl is required to verify the Authenticode signature independently'
    authenticode_content "$signature_blob" "$signed_content" ||
        fail "cannot find SpcIndirectDataContent in the Authenticode signature of $signed_driver"
    if ! openssl smime -verify -inform DER -in "$signature_blob" -noverify \
            -content "$signed_content" -out /dev/null >/dev/null 2>&1; then
        fail "the Authenticode signature does not verify against the signed attributes in $signed_driver"
    fi

    signer_subject="$(awk '/Signer #0:/ { seen = 1 } seen && /Subject:/ {
        sub(/^.*Subject: */, ""); print; exit }' "$log")"
    [[ -n "$signer_subject" ]] || fail 'could not read the signer certificate subject'
    signer_cn="$(printf '%s\n' "$signer_subject" | sed -n 's/.*CN=\(.*\)/\1/p')"
    [[ -n "$signer_cn" ]] || fail "could not read CN from the signer subject: $signer_subject"

    if command -v openssl >/dev/null; then
        expected_cn="$(openssl pkcs12 -in "$pfx" -nokeys -passin "pass:$password" -nodes 2>/dev/null |
            openssl x509 -noout -subject 2>/dev/null |
            sed -n 's/.*CN=\(.*\)/\1/p')"
        if [[ -n "$expected_cn" ]]; then
            [[ "$signer_cn" == "$expected_cn" ]] ||
                fail "signed by CN=\"$signer_cn\", expected CN=\"$expected_cn\""
        fi
    fi

    if (( rc == 0 )); then
        signature_status="ok (sha256 digest, PKCS#7 signature and chain verified; signer CN \"$signer_cn\")"
        return 0
    fi

    # The WDK test root is not a CA for OpenSSL.  That one trust-anchor failure
    # is expected; every other failure (bad signature, corrupt p7, timestamp,
    # unknown issuer outside the test chain) is fatal.
    grep -q 'Number of verified signatures: 1' "$log" ||
        fail 'osslsigncode did not count exactly one verified signature'
    if ! grep -qE 'Verify error: (invalid CA certificate|unable to get local issuer certificate|unable to get issuer certificate)' "$log"; then
        sed -n '1,20p' "$log" >&2
        fail "osslsigncode verify failed for a reason other than the WDK test root"
    fi
    unexpected="$(grep -E 'error:|Error:' "$log" | grep -vE \
        'invalid CA certificate|unable to get local issuer certificate|unable to get issuer certificate' || true)"
    if [[ -n "$unexpected" ]]; then
        printf '%s\n' "$unexpected" >&2
        fail 'osslsigncode verify reported unexpected errors'
    fi

    signature_status="ok (sha256 digest and PKCS#7 signature verified; signer CN \"$signer_cn\"; WDK test chain trusted by Windows only)"
}

# ---------------------------------------------------------------------------
# PE verification
# ---------------------------------------------------------------------------

exe_import_list=''
exe_subsystem=''
verify_native_exe() {
    local expected='ntdll.dll'
    exe_subsystem="$(pe_subsystem "$native_exe")"
    [[ "$exe_subsystem" == 'IMAGE_SUBSYSTEM_NATIVE' ]] ||
        fail "$native_exe subsystem is $exe_subsystem, expected IMAGE_SUBSYSTEM_NATIVE"
    exe_import_list="$(pe_imports "$native_exe")"
    [[ -n "$exe_import_list" ]] || fail "no imports found in $native_exe"
    [[ "$exe_import_list" == "$expected" ]] ||
        fail "$native_exe imports [$exe_import_list], expected only $expected"
}

driver_import_list=''
verify_driver_image() {
    local allowed=' hal.dll ntoskrnl.exe '
    local import subsystem
    subsystem="$(pe_subsystem "$signed_driver")"
    [[ "$subsystem" == 'IMAGE_SUBSYSTEM_NATIVE' ]] ||
        fail "$signed_driver subsystem is $subsystem, expected IMAGE_SUBSYSTEM_NATIVE"
    driver_import_list="$(pe_imports "$signed_driver")"
    [[ -n "$driver_import_list" ]] || fail "no imports found in $signed_driver"
    while read -r import; do
        [[ -n "$import" ]] || continue
        # lld reports HAL.dll in upper case; compare case-insensitively.
        case "$allowed" in
            *" ${import,,} "*) ;;
            *) fail "$signed_driver imports $import, expected only kernel modules" ;;
        esac
    done < <(pe_import_list "$signed_driver")
}

audio_status='absent'
verify_audio_object() {
    local defined required undefined symbol
    [[ -f "$audio_object" ]] || return 0

    "$llvm_bin/llvm-nm" --defined-only --extern-only "$audio_object" >/dev/null 2>&1 ||
        fail "$audio_object is not a valid object file"
    defined="$("$llvm_bin/llvm-nm" --defined-only --extern-only "$audio_object" |
        awk '{ print $NF }' | LC_ALL=C sort -u)"
    for required in Win0DoomAudioInitialize Win0DoomAudioShutdown Win0DoomAudioDeviceControl; do
        grep -qx -- "$required" <<<"$defined" ||
            fail "$audio_object does not define $required"
    done

    undefined="$("$llvm_bin/llvm-nm" --undefined-only "$display_object" |
        awk '{ print $NF }' | grep '^Win0DoomAudio' | LC_ALL=C sort -u || true)"
    [[ -n "$undefined" ]] ||
        fail "$audio_object exists but win0doom-display.obj references none of the Win0DoomAudio* entry points; the driver was linked without audio"
    for symbol in $undefined; do
        grep -qx -- "$symbol" <<<"$defined" ||
            fail "win0doom-display.obj references $symbol, which $audio_object does not define"
    done

    [[ ! "$signed_driver" -ot "$audio_object" ]] ||
        fail "$signed_driver is older than $audio_object; the audio code was not linked"

    audio_status="linked ($(file_size "$audio_object") byte object, $(printf '%s\n' "$undefined" | wc -l) entry points referenced)"
}

say ''
say '== verify =='
verify_signature
verify_native_exe
verify_driver_image
verify_audio_object
say "signature: $signature_status"
say "imports: $native_exe -> $exe_import_list"
say "imports: $signed_driver -> $driver_import_list"
say "audio: $audio_status"

# ---------------------------------------------------------------------------
# checksums + manifest
# ---------------------------------------------------------------------------

sha256sum \
    "$native_exe" \
    "$probe_exe" \
    "$signed_driver" \
    > "$sha_file"

probe_subsystem="$(pe_subsystem "$probe_exe")"
probe_imports="$(pe_imports "$probe_exe")"

{
    say 'release manifest'
    say '----------------'
    printf '%-34s %8s bytes  sha256 %s\n' \
        'native/build/win0doom.exe' "$(file_size "$native_exe")" "$(file_sha256 "$native_exe")"
    printf '  subsystem %s, imports %s\n' "$exe_subsystem" "$exe_import_list"
    printf '%-34s %8s bytes  sha256 %s\n' \
        'native/build/inputprobe.exe' "$(file_size "$probe_exe")" "$(file_sha256 "$probe_exe")"
    printf '  subsystem %s, imports %s\n' "$probe_subsystem" "$probe_imports"
    printf '%-34s %8s bytes  sha256 %s\n' \
        'driver/build/win0doom-display-signed.sys' "$(file_size "$signed_driver")" "$(file_sha256 "$signed_driver")"
    printf '  signature %s\n' "$signature_status"
    printf '  subsystem %s, imports %s\n' 'IMAGE_SUBSYSTEM_NATIVE' "$driver_import_list"
    printf '  audio object %s\n' "$audio_status"
    say ''
    say "checksums: driver/build/release.sha256"
    say "manifest:  driver/build/release-manifest.txt"
} | tee "$manifest_file"

say ''
say 'release artifacts:'
cat "$sha_file"
