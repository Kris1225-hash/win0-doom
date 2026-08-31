#!/usr/bin/env bash
set -euo pipefail

driver_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$driver_dir/build"
sdk_root="${WINDOWS_SDK_ROOT:-$driver_dir/../toolchain/wdk-26100/c}"
sdk_version="10.0.26100.0"
llvm_bin="/usr/lib/llvm/22/bin"

mkdir -p "$build_dir"

"$llvm_bin/clang-cl" \
  /c /nologo /kernel /GS- /GR- /Zl /W4 \
  /clang:-Wno-nonportable-include-path \
  /clang:-Wno-unknown-pragmas \
  /clang:-Wno-ignored-pragma-intrinsic \
  /clang:-Wno-pragma-pack \
  /D_AMD64_ /D_WIN64 /D_KERNEL_MODE \
  /I"$sdk_root/Include/$sdk_version/km/crt" \
  /I"$sdk_root/Include/$sdk_version/shared" \
  /I"$sdk_root/Include/$sdk_version/km" \
  /Fo"$build_dir/win0doom-display.obj" \
  "$driver_dir/win0doom-display.c"

"$llvm_bin/lld-link" \
  /nologo /driver /release /machine:x64 /subsystem:native \
  /entry:DriverEntry /nodefaultlib \
  /out:"$build_dir/win0doom-display.sys" \
  "$build_dir/win0doom-display.obj" \
  "$sdk_root/Lib/$sdk_version/km/x64/ntoskrnl.lib" \
  "$sdk_root/Lib/$sdk_version/km/x64/hal.lib"

"$llvm_bin/llvm-readobj" --file-headers --coff-imports "$build_dir/win0doom-display.sys"
