#!/usr/bin/env bash
set -euo pipefail

native_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$native_dir/build"
sdk_root="${WINDOWS_SDK_ROOT:-$native_dir/../toolchain/wdk-26100/c}"
sdk_version="10.0.26100.0"
llvm_bin="/usr/lib/llvm/22/bin"

mkdir -p "$build_dir"

"$llvm_bin/clang-cl" \
  /c /nologo /O2 /GS- /GR- /Zl /W1 \
  /clang:-Wno-nonportable-include-path \
  /clang:-Wno-unknown-pragmas \
  /clang:-Wno-ignored-pragma-intrinsic \
  /clang:-Wno-pragma-pack \
  /clang:-fno-builtin-memset \
  /clang:-fno-builtin-memcpy \
  /clang:-fno-builtin-strlen \
  /D_AMD64_ /D_WIN64 /DUNICODE /D_UNICODE \
  /I"$sdk_root/Include/$sdk_version/km/crt" \
  /I"$sdk_root/Include/$sdk_version/shared" \
  /I"$sdk_root/Include/$sdk_version/um" \
  /I"$sdk_root/Include/$sdk_version/ucrt" \
  /I"$native_dir/.." \
  /Fo"$build_dir/win0-doom.obj" \
  "$native_dir/win0-doom.c"

"$llvm_bin/lld-link" \
  /nologo /machine:x64 /subsystem:native \
  /entry:NtProcessStartup /nodefaultlib \
  /stack:4194304,65536 \
  /out:"$build_dir/win0doom.exe" \
  "$build_dir/win0-doom.obj" \
  "$sdk_root/um/x64/ntdll.lib"

"$llvm_bin/llvm-readobj" --file-headers --coff-imports "$build_dir/win0doom.exe"
