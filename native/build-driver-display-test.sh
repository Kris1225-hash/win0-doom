#!/usr/bin/env bash
set -euo pipefail

native_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$native_dir/build"
sdk_root="${WINDOWS_SDK_ROOT:-$native_dir/../toolchain/wdk-26100/c}"
sdk_version="10.0.26100.0"
llvm_bin="/usr/lib/llvm/22/bin"

mkdir -p "$build_dir"

"$llvm_bin/clang-cl" \
  /c /nologo /GS- /GR- /Zl /W3 \
  /clang:-Wno-nonportable-include-path \
  /clang:-Wno-unknown-pragmas \
  /clang:-Wno-ignored-pragma-intrinsic \
  /clang:-Wno-pragma-pack \
  /D_AMD64_ /D_WIN64 /DUNICODE /D_UNICODE \
  /I"$sdk_root/Include/$sdk_version/km/crt" \
  /I"$sdk_root/Include/$sdk_version/shared" \
  /I"$sdk_root/Include/$sdk_version/um" \
  /I"$sdk_root/Include/$sdk_version/ucrt" \
  /Fo"$build_dir/driver-display-test.obj" \
  "$native_dir/driver-display-test.c"

"$llvm_bin/lld-link" \
  /nologo /machine:x64 /subsystem:native \
  /entry:NtProcessStartup /nodefaultlib \
  /out:"$build_dir/driver-display-test.exe" \
  "$build_dir/driver-display-test.obj" \
  "$sdk_root/um/x64/ntdll.lib"

"$llvm_bin/llvm-readobj" --file-headers --coff-imports "$build_dir/driver-display-test.exe"
