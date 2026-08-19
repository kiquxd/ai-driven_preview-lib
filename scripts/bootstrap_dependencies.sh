#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -s):$(uname -m)" in
  Linux:x86_64|Darwin:arm64) ;;
  *) printf 'supported hosts: Linux x86-64, macOS arm64\n' >&2; exit 1 ;;
esac

for header in stb_image.h stb_image_resize2.h; do
  if [[ ! -f "${project_root}/third_party/stb/${header}" ]]; then
    printf 'missing vendored dependency: third_party/stb/%s\n' "${header}" >&2
    exit 1
  fi
done

printf 'Dependencies are ready (stb is vendored; no download is required).\n'
