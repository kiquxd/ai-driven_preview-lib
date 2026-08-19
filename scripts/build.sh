#!/usr/bin/env bash
set -euo pipefail

variant="${1:-dev}"
case "${variant}" in
  dev|asan|tsan) ;;
  *)
    printf 'usage: %s [dev|asan|tsan]\n' "$0" >&2
    exit 2
    ;;
esac

case "$(uname -s):$(uname -m)" in
  Linux:x86_64) preset="${variant}" ;;
  Darwin:arm64) preset="mac-${variant}" ;;
  *)
    printf 'supported hosts: Linux x86_64, macOS arm64\n' >&2
    exit 1
    ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache="${root}/build/${preset}/CMakeCache.txt"
fresh=false
if [[ -f "${cache}" ]]; then
  cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache}")"
  cached_pdfium_out="$(sed -n 's/^PREVIEW_PDFIUM_OUT:PATH=//p' "${cache}")"
  if [[ "${cached_source}" != "${root}" ||
        "${cached_pdfium_out}" == */out/Preview ]]; then
    printf 'Refreshing a relocated or legacy CMake cache.\n'
    fresh=true
  fi
fi

if [[ "${fresh}" == true ]]; then
  cmake --fresh --preset "${preset}"
else
  cmake --preset "${preset}"
fi
cmake --build --preset "${preset}"
ctest --preset "${preset}"
