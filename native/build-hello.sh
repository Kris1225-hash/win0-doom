#!/usr/bin/env bash
set -euo pipefail

native_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$native_dir/build"
llvm_bin="/usr/lib/llvm/22/bin"
output_name="${HELLO_OUTPUT_NAME:-hello.exe}"
link_mode_args=()

if [[ "${HELLO_FIXED:-0}" == 1 ]]; then
  link_mode_args=(/fixed /dynamicbase:no /highentropyva:no)
fi

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
  /W4 \
  /WX \
  /Fo"$build_dir/hello.obj" \
  "$native_dir/hello.c"

"$llvm_bin/lld-link" \
  /nologo \
  /machine:x64 \
  /subsystem:native,10.0 \
  /osversion:10.0 \
  /version:10.0 \
  /stack:524288,12288 \
  /heap:1048576,4096 \
  /release \
  /entry:NtProcessStartup \
  /nodefaultlib \
  "${link_mode_args[@]}" \
  /out:"$build_dir/$output_name" \
  "$build_dir/hello.obj" \
  "$build_dir/ntdll.lib"

"$llvm_bin/llvm-objdump" -p "$build_dir/$output_name"
