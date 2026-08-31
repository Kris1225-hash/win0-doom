#!/usr/bin/env bash
set -euo pipefail

native_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$native_dir/build"
llvm_bin="/usr/lib/llvm/22/bin"

mkdir -p "$build_dir"

"$llvm_bin/llvm-dlltool" \
  -m i386:x86-64 \
  -d "$native_dir/ntdll.def" \
  -l "$build_dir/ntdll.lib"

"$llvm_bin/clang-cl" \
  /c \
  /nologo \
  /GS- \
  /GR- \
  /Zl \
  /W0 \
  /I"$native_dir/.." \
  /Fo"$build_dir/engine-link-test.obj" \
  "$native_dir/engine-link-test.c"

"$llvm_bin/lld-link" \
  /nologo \
  /machine:x64 \
  /subsystem:native \
  /entry:NtProcessStartup \
  /nodefaultlib \
  /out:"$build_dir/engine-link-test.exe" \
  "$build_dir/engine-link-test.obj" \
  "$build_dir/ntdll.lib"

"$llvm_bin/llvm-objdump" -p "$build_dir/engine-link-test.exe"
