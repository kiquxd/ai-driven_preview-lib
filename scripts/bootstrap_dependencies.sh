#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_root="${project_root}/.deps"
depot_tools="${deps_root}/depot_tools"
pdfium_work="${deps_root}/pdfium-work"
pdfium_root="${pdfium_work}/pdfium"
tools_prefix="${deps_root}/tools"

pdfium_revision="78adc7c30182d34921cbe6ec0fe9beb2ea94d200"
depot_tools_revision="ce5a30784f7323ee224f3a6e7d4ae208dd6d17fe"
pkgconf_version="2.3.0"
pkgconf_sha256="3a9080ac51d03615e7c1910a0a2a8df08424892b5f13b0628a204d3fcce0ea8b"

host_os="$(uname -s)"
host_arch="$(uname -m)"
case "${host_os}:${host_arch}" in
  Linux:x86_64)
    platform_tag="linux-x64"
    target_os="linux"
    target_cpu="x64"
    ;;
  Darwin:arm64)
    platform_tag="mac-arm64"
    target_os="mac"
    target_cpu="arm64"
    if ! command -v xcrun >/dev/null 2>&1 ||
       ! command -v xcodebuild >/dev/null 2>&1; then
      printf 'Full Xcode is required to build PDFium on macOS.\n' >&2
      exit 1
    fi
    if ! xcodebuild -version >/dev/null 2>&1; then
      printf 'Full Xcode is required; Command Line Tools alone are insufficient.\n' >&2
      printf 'After installing Xcode, select it with:\n' >&2
      printf '  sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer\n' >&2
      printf 'Then complete setup with:\n' >&2
      printf '  sudo xcodebuild -runFirstLaunch\n' >&2
      exit 1
    fi
    xcrun --sdk macosx --show-sdk-path >/dev/null
    ;;
  *)
    printf 'unsupported build host: %s %s\n' "${host_os}" "${host_arch}" >&2
    printf 'supported hosts: Linux x86_64, macOS arm64\n' >&2
    exit 1
    ;;
esac
pdfium_out="out/Preview-${platform_tag}"

mkdir -p "${deps_root}"
if [[ ! -d "${depot_tools}/.git" ]]; then
  git clone --depth=1 \
    https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "${depot_tools}"
fi
git -C "${depot_tools}" fetch --depth=1 origin "${depot_tools_revision}"
git -C "${depot_tools}" checkout --detach "${depot_tools_revision}"
export PATH="${tools_prefix}/bin:${depot_tools}:${PATH}"
export DEPOT_TOOLS_UPDATE=0
export GCLIENT_SUPPRESS_GIT_VERSION_WARNING=1

# A freshly cloned pinned depot_tools checkout does not yet contain the CIPD
# Python runtime metadata used by gclient's wrappers. Auto-update is disabled
# intentionally, so initialize the tools explicitly without moving the pinned
# Git revision.
DEPOT_TOOLS_DIR="${depot_tools}" "${depot_tools}/ensure_bootstrap"

if [[ "${host_os}" == "Linux" && ! -x "${tools_prefix}/bin/pkgconf" ]]; then
  archive="${deps_root}/pkgconf-${pkgconf_version}.tar.xz"
  source_dir="${deps_root}/pkgconf-${pkgconf_version}"
  if [[ ! -f "${archive}" ]]; then
    curl -fsSL \
      "https://distfiles.ariadne.space/pkgconf/pkgconf-${pkgconf_version}.tar.xz" \
      -o "${archive}"
  fi
  printf '%s  %s\n' "${pkgconf_sha256}" "${archive}" | sha256sum --check
  tar -xf "${archive}" -C "${deps_root}"
  (
    cd "${source_dir}"
    ./configure --prefix="${tools_prefix}" --disable-shared \
      --with-system-includedir=/usr/include \
      --with-system-libdir=/usr/lib/x86_64-linux-gnu
    make -j"$(getconf _NPROCESSORS_ONLN)"
    make install
  )
fi
if [[ "${host_os}" == "Linux" ]]; then
  ln -sf pkgconf "${tools_prefix}/bin/pkg-config"
fi

mkdir -p "${pdfium_work}"
if [[ ! -f "${pdfium_work}/.gclient" ]]; then
  (
    cd "${pdfium_work}"
    gclient config --name=pdfium --unmanaged \
      --custom-var=checkout_configuration=minimal \
      https://pdfium.googlesource.com/pdfium.git
  )
fi
(
  cd "${pdfium_work}"
  gclient sync --revision="pdfium@${pdfium_revision}" --no-history --nohooks
  gclient runhooks
)

gn_args="target_os=\"${target_os}\"
target_cpu=\"${target_cpu}\"
is_debug=false
is_component_build=false
is_clang=true
use_custom_libcxx=false
clang_use_chrome_plugins=false
use_remoteexec=false
symbol_level=0
treat_warnings_as_errors=false
pdf_is_complete_lib=true
pdf_enable_v8=false
pdf_enable_xfa=false
pdf_use_partition_alloc=false
pdf_use_agg=true
pdf_use_skia=false
pdf_enable_brotli=false
pdf_enable_fontations=false
pdf_enable_rust_bmp=false
pdf_enable_rust_jpeg=false
pdf_enable_rust_png=false
pdf_bundle_freetype=true
use_system_libjpeg=false
use_libjpeg_turbo=true
use_system_lcms2=false
use_system_libopenjpeg2=false
use_system_libpng=false
use_system_libtiff=false
use_system_zlib=false"

if [[ "${host_os}" == "Linux" ]]; then
  gn_args+=$'\nuse_sysroot=true'
else
  gn_args+=$'\nuse_sysroot=false\nuse_system_xcode=true\nmac_deployment_target="13.0"'
fi

(
  cd "${pdfium_root}"
  gn gen "${pdfium_out}" --check --args="${gn_args}"
  autoninja -C "${pdfium_out}" pdfium
)

if [[ ! -f "${pdfium_root}/${pdfium_out}/obj/libpdfium.a" ]]; then
  printf 'PDFium archive was not produced at %s\n' \
    "${pdfium_root}/${pdfium_out}/obj/libpdfium.a" >&2
  exit 1
fi

actual_revision="$(git -C "${pdfium_root}" rev-parse HEAD)"
if [[ "${actual_revision}" != "${pdfium_revision}" ]]; then
  printf 'unexpected PDFium revision: %s\n' "${actual_revision}" >&2
  exit 1
fi
printf 'PDFium %s is ready at %s\n' "${actual_revision}" "${pdfium_root}"
printf 'Platform output: %s\n' "${pdfium_out}"
