#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${WINDOWS_SDK_ROOT:-$repo_dir/toolchain/wdk-26100/c}"
signer="${OSSLSIGNCODE:-$repo_dir/toolchain/osslsigncode/build/osslsigncode}"
pfx="${WIN0DOOM_SIGN_PFX:-$sdk_root/tools/certificates/OEM_Test_Cert_2017.pfx}"
intermediate="${WIN0DOOM_SIGN_INTERMEDIATE:-$sdk_root/tools/certificates/OEM_Intermediate_Cert_2017.cer}"
signed_driver="$repo_dir/driver/build/win0doom-display-signed.sys"
signed_temporary="$repo_dir/driver/build/.win0doom-display-signed.$$.sys"

for required in "$signer" "$pfx" "$intermediate"; do
    if [[ ! -f "$required" && ! -x "$required" ]]; then
        printf 'required build or signing file is missing: %s\n' "$required" >&2
        exit 2
    fi
done

cleanup() {
    if [[ -e "$signed_temporary" ]]; then
        mv -f -- "$signed_temporary" "$signed_temporary.failed"
    fi
}
trap cleanup EXIT

WINDOWS_SDK_ROOT="$sdk_root" "$repo_dir/native/build-win0-doom.sh"
WINDOWS_SDK_ROOT="$sdk_root" "$repo_dir/native/build-input-probe.sh"
WINDOWS_SDK_ROOT="$sdk_root" "$repo_dir/driver/build-driver.sh"

"$signer" sign \
    -pkcs12 "$pfx" \
    -pass "${WIN0DOOM_SIGN_PASSWORD:-}" \
    -ac "$intermediate" \
    -h sha256 \
    -n 'Win0 Doom display and keyboard bridge' \
    -in "$repo_dir/driver/build/win0doom-display.sys" \
    -out "$signed_temporary"
mv -f -- "$signed_temporary" "$signed_driver"

sha256sum \
    "$repo_dir/native/build/win0doom.exe" \
    "$repo_dir/native/build/inputprobe.exe" \
    "$signed_driver" \
    > "$repo_dir/driver/build/release.sha256"

printf '\nrelease artifacts:\n'
cat "$repo_dir/driver/build/release.sha256"
