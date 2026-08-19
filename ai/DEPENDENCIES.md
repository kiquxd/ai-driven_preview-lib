# preview_lib dependencies

## 1. Dependency policy

`preview_lib` produces one runtime shared library: `libpreview.so` on Linux or
`libpreview.dylib` on macOS. Consumers include one public header and link one
CMake target:

```cpp
#include <preview/preview.hpp>
```

```cmake
target_link_libraries(app PRIVATE preview::preview)
```

Rules:

- No third-party type or header is exposed by `preview/preview.hpp`.
- No third-party shared library is shipped next to `libpreview`.
- PDFium and its required codec/rendering closure are built as position-
  independent static code and folded into the platform `libpreview` binary.
- The standalone JPEG provider reuses PDFium's pinned bundled
  `libjpeg-turbo`; a second JPEG implementation is forbidden.
- The two stb headers are compiled directly into private translation units.
- Ordinary consumer configuration never downloads dependencies.
- Every source dependency is pinned by immutable commit/tag plus checksum.
- The actual linked closure, revisions and license texts are generated from the
  selected build and recorded before release.
- System libraries/frameworks supplied by Linux or macOS are allowed and
  audited with `readelf`/`ldd` or `otool`/`nm`; they are not additional project
  artifacts.
- WebP support and `libwebp` are explicitly excluded.

Milestone 0 selected immutable revisions. The authoritative machine-readable
values and header checksums are in `third_party/manifest.cmake`; never replace
them with a moving `main`/`master` reference.

## 2. Direct production dependencies

### 2.1 C++20 standard library

Purpose:

- public value types and ownership;
- `std::span`, `std::variant`, `std::stop_token`, `std::filesystem`;
- strings, vectors, synchronization and checked wrapper code.

Policy:

- compile the library and all C++ dependencies with one compatible toolchain;
- Linux uses Clang plus libstdc++; macOS uses Clang plus the Xcode SDK/libc++;
- do not bundle a second C++ standard library accidentally through PDFium;
- no stable ABI is promised across different compiler/standard-library pairs.

Expected dynamic runtime entries include the selected C++ runtime, compiler
unwind/runtime library and glibc. The final list is an allowlist produced from
the installed platform `libpreview` binary, not guessed in advance.

### 2.2 PDFium

Repository:

- <https://pdfium.googlesource.com/pdfium/>

Revision:

- `78adc7c30182d34921cbe6ec0fe9beb2ea94d200`.

License:

- PDFium top-level code uses a BSD-style license;
- portions and transitive components carry their own permissive licenses;
- exact upstream `LICENSE`/`NOTICE` material must be copied from the pinned
  checkout into the generated `THIRD_PARTY_NOTICES`;
- PDFium does not impose AGPL/GPL copyleft on `preview_lib` when built with the
  dependency set described here.

Used functionality:

- `FPDF_InitLibraryWithConfig` / library shutdown;
- `FPDF_FILEACCESS` and `FPDF_LoadCustomDocument` for `ByteSource` range reads;
- page count, page dimensions, rotation and document metadata;
- `FPDF_LoadPage` and `FPDF_ClosePage`;
- `FPDFBitmap_CreateEx` with a caller-owned BGRA buffer;
- page rendering at the requested target size;
- progressive rendering API for cooperative cancellation;
- `FPDF_GetLastError` for invalid/encrypted/password-protected documents.

Not used:

- V8 or PDF JavaScript;
- XFA forms;
- Skia renderer;
- browser integration;
- editing APIs;
- PDFium sample programs, tests or tools;
- Rust image decoders/fontations;
- Brotli PDF experiments;
- PartitionAlloc in the initial minimal build.

Candidate GN configuration to validate and freeze in milestone 0:

```text
is_debug = false
is_component_build = false
pdf_is_complete_lib = true
pdf_enable_v8 = false
pdf_enable_xfa = false
pdf_use_skia = false
pdf_use_agg = true
pdf_use_partition_alloc = false
pdf_enable_brotli = false
pdf_enable_fontations = false
pdf_enable_rust_bmp = false
pdf_enable_rust_jpeg = false
pdf_enable_rust_png = false
pdf_bundle_freetype = true
use_system_libjpeg = false
use_libjpeg_turbo = true
use_system_libopenjpeg2 = false
use_system_libpng = false
use_system_zlib = false
clang_use_chrome_plugins = false
use_remoteexec = false
symbol_level = 0
```

`pdf_is_complete_lib = true` is important: the `//:pdfium` target must be a
complete static library suitable for folding into the platform `preview`
shared library (`libpreview.so` or `libpreview.dylib`).

The spike must verify every flag against the chosen PDFium revision. PDFium's GN
arguments evolve; an unknown or silently ignored flag is a build failure for our
purposes.

Operational constraints:

- PDFium's public API is not thread-safe; all engines share one process-global
  lifetime object and mutex.
- PDFium runs in the caller process; there is no decoder sandbox.
- PDFium does not expose a complete allocator budget. Input reads, output bitmap
  and cache are bounded, but internal parser memory is not under a hard cap.
- The dependency must be updated promptly for upstream security fixes.

### 2.3 PDFium-bundled libjpeg-turbo

Repository used by PDFium:

- the `third_party/libjpeg_turbo` revision recorded in the pinned PDFium `DEPS`.

Revision:

- `640f254ad0fa03f6b1f29f89b7dd9366f2f6e533`, derived from the selected
  PDFium commit; it is not independently rolled.

License:

- IJG license plus modified BSD-style terms and included component notices;
- preserve `LICENSE.md`, `README.ijg` and the required attribution statement in
  `THIRD_PARTY_NOTICES`.

Why it is used twice but linked once:

- PDFium needs it to decode JPEG objects embedded in PDFs;
- `preview_lib` needs it to decode standalone JPEG files;
- both users call into the same compiled libjpeg implementation contained in
  the prepared PDFium dependency closure;
- Chromium's `jpeglibmangler.h` maps the standalone provider's calls to the
  `chromium_jpeg_*` symbols present in the complete archive;
- linking a separate `libjpeg-turbo` build would risk duplicate global symbols,
  version skew and unnecessary binary size.

Standalone JPEG provider API:

- use the libjpeg API, including libjpeg-turbo scaling/color-space extensions;
- inspect dimensions before output allocation;
- use IDCT scaling to approach the requested viewport size;
- decode to BGRA-compatible four-channel output where supported;
- perform only the final exact resize with `stb_image_resize2`;
- disable JPEG inside `stb_image`.

Do not depend on the TurboJPEG tools or install `cjpeg`, `djpeg`, `jpegtran`,
`tjbench` or any other executable.

### 2.4 stb_image.h

Repository:

- <https://github.com/nothings/stb>

Revision:

- commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`;
- `stb_image.h` SHA-256
  `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.

License:

- public-domain dedication or MIT dual-license; use the MIT option in the
  manifest and include its text in `THIRD_PARTY_NOTICES`.

Purpose:

- decode PNG;
- decode the first frame of GIF;
- decode BMP.

Private implementation configuration:

```cpp
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
```

The implementation macro is defined in exactly one private `.cpp`. Confirm that
the selected stb revision supports combining the required `STBI_ONLY_*` macros.
JPEG, WebP, PSD, TGA, HDR, PIC and PNM are not compiled.

Memory callbacks should be overridden with `STBI_MALLOC`, `STBI_REALLOC` and
`STBI_FREE` so library-owned working-memory accounting is enforced where stb
permits it. `stb_image` has no general mid-decode cancellation hook; cancellation
is checked before and after the call.

For GIF, use the ordinary first-image decode path. Do not use an API that eagerly
decodes every animation frame.

### 2.5 stb_image_resize2.h

Repository:

- <https://github.com/nothings/stb>

Revision:

- commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`;
- `stb_image_resize2.h` SHA-256
  `173e654634f6ccaad98f603e686ea212eec1fe8ea6d2a5e5e8056efa10ae3880`.

License:

- public-domain dedication or MIT dual-license; use MIT in the manifest.

Purpose:

- final resize of decoded image pixels to the requested viewport;
- resize after EXIF orientation where needed;
- avoid exposing decoder-specific pixel types.

Configuration:

- implementation in exactly one private `.cpp`;
- operate on four-channel BGRA/RGBA buffers with alpha in the documented channel;
- use sRGB-aware resize entry points where appropriate;
- account for resize scratch/intermediate memory under `max_working_bytes`.

## 3. PDFium transitive dependency closure

PDFium is a framework-sized dependency even in the reduced build. Its top-level
`DEPS` file contains build tools, tests and optional subsystems that are not all
linked into `//:pdfium`. Therefore the release manifest must distinguish:

1. fetched/build-only dependencies;
2. code actually present in the complete static PDFium archive;
3. dynamic system libraries or frameworks needed by the final platform shared
   library.

Expected linked components in the no-V8/no-XFA/no-Skia AGG build include:

| Component | Purpose | Expected handling |
|---|---|---|
| PDFium core/fpdfsdk/fxcrt | PDF parsing and public API | folded into the platform `preview` shared library |
| AGG 2.3 code | vector rasterization | bundled by PDFium |
| FreeType | PDF font rasterization | bundled PDFium copy |
| Little CMS (lcms2) | color management | bundled PDFium copy |
| OpenJPEG | JPEG 2000 images in PDF | bundled PDFium copy |
| libjpeg-turbo | JPEG in PDF and standalone JPEG | one bundled copy |
| libpng | PNG support used by PDFium paths | bundled PDFium copy when linked |
| zlib | Flate/PNG decompression | bundled PDFium copy |
| PDFium bigint/fast-float/dragonbox helpers | parser/runtime helpers | bundled when reachable |
| Abseil subsets | internal utility code | bundled when reachable |
| Fontconfig | Linux font discovery/substitution | normally a system dynamic library |

Possible repositories present in PDFium `DEPS` but expected to be excluded by
our flags include V8, ICU-for-V8, Skia, Highway-for-V8/Skia, PartitionAlloc,
XFA-only TIFF/image codecs, test corpora, GoogleTest and benchmark tooling.

This table is a planning expectation, not the legal manifest. After GN generation
the agent must capture the exact closure with commands equivalent to:

```bash
gn desc out/preview_pdfium //:pdfium deps --all --tree
gn desc out/preview_pdfium //:pdfium libs --all
```

Then inspect the resulting complete archive/final shared object with:

```bash
readelf -d libpreview.so
readelf --dyn-syms libpreview.so
nm -D --defined-only libpreview.so
ldd libpreview.so
```

Every linked third-party component must have an immutable revision, source URL,
checksum and license entry in the production dependency manifest. Components
merely fetched for building must be recorded separately but must not be described
as runtime dependencies.

## 4. Linux system dependencies

These are not vendored or shipped as project libraries.

Expected runtime/build interface:

- glibc (`libc`, and platform-dependent `libm`, `libdl`, `libpthread` behavior);
- selected C++ runtime and compiler unwind/runtime library;
- Fontconfig on Linux for PDF font discovery and substitution;
- Fontconfig's system-side dependencies, commonly FreeType and Expat;
- the host font database/configuration used by Fontconfig.

The milestone-0 direct `DT_NEEDED` allowlist on Linux x86-64 is
`libfontconfig.so.1`, `libdl.so.2`, `libm.so.6`, `libpthread.so.0`,
`libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6`, and the ELF loader. On the
reference host Fontconfig's transitive system closure additionally resolves
FreeType, Expat, zlib, bzip2, libpng and Brotli. These transitive libraries are
owned by the system Fontconfig/FreeType installation, not separately linked
image providers. An unexpected direct dependency such as `libpdfium.so`,
`libjpeg.so`, `libwebp.so`, `libpoppler.so` or `libmupdf.so` fails packaging
verification.

Font availability affects PDF visual fidelity. Golden PDF tests must use embedded
fonts where possible; tests depending on substitution must provide a controlled
font configuration rather than assuming the developer workstation's fonts.

### 4.1 macOS system dependencies

The macOS arm64 build uses system frameworks declared by the pinned PDFium
graph: AppKit, CoreFoundation, and CoreGraphics. It also uses the normal macOS
libc++/system runtime closure. These frameworks are linked into
`libpreview.dylib`; they are not bundled or exposed as separate project
artifacts. `otool -L`, `file`, and `nm -gU` replace the Linux binary-audit tools.

The deployment target is macOS 13.0 and the binary contains only an arm64 slice.

## 5. Build-only dependencies

### Main library

- CMake: configure, build, install and package export;
- Clang/LLVM: preferred unified compiler for PDFium and `preview_lib`;
- a linker supporting hidden visibility and archive symbol exclusion (`lld` or
  GNU `ld` as validated in milestone 0);
- Ninja: primary build executor;
- Python 3: PDFium/Chromium build scripts;
- Git: obtaining pinned source revisions during explicit developer bootstrap;
- pkgconf and Fontconfig development metadata on Linux only;
- full Xcode and its macOS SDK on macOS arm64 (standalone Command Line Tools
  are insufficient for PDFium's `xcodebuild`-based SDK discovery);
- NASM/Yasm on x86/x86-64 if required by the selected libjpeg-turbo SIMD build.

### PDFium bootstrap/build

- Chromium `depot_tools`, pinned or recorded for reproducibility;
- `gclient` for the selected minimal PDFium checkout;
- GN for PDFium build generation;
- Ninja/Autoninja for the complete static target;
- the PDFium-supported Clang toolchain or a deliberately validated host Clang;
- standard Unix build tools and Python modules requested by the pinned checkout.

These tools are not linked into `libpreview`, are not installed for consumers
and must not be fetched by `find_package(preview)` or `add_subdirectory`.

## 6. Test-only dependencies

The test suite deliberately avoids a third-party unit-test framework initially.

Required:

- CTest plus the in-tree minimal test harness;
- ASan and UBSan runtimes supplied by the compiler;
- TSan when compatible with the selected PDFium build/toolchain;
- `readelf`, `nm`, and `ldd` on Linux or `file`, `nm`, and `otool` on macOS for
  binary audits;
- small repository-owned or redistribution-safe image/PDF fixtures.

Optional development tooling:

- Clang libFuzzer for native metadata/text parsers;
- llvm-symbolizer for sanitizer diagnostics;
- `perf` or equivalent profiler for investigation only.

No benchmark framework, network service, SSH server, TUI library or database is
required to test `preview_lib`.

## 7. Explicitly forbidden/not used dependencies

- MuPDF;
- Poppler;
- Ghostscript;
- PDF.js or a JavaScript runtime;
- Apache PDFBox or a JVM;
- ImageMagick/GraphicsMagick;
- OpenCV;
- libvips;
- Qt, SDL or any GUI toolkit;
- ncurses, notcurses, FTXUI or another TUI toolkit;
- libwebp/WebP codecs;
- libssh/libssh2/OpenSSH/SFTP/SCP code;
- a thread-pool/event-loop library;
- a logging framework;
- a cache/database library;
- a third-party expected/result library;
- an external test framework for MVP.

## 8. Installed artifacts and dependency visibility

Installed product files may include:

```text
lib/libpreview.so
lib/libpreview.dylib  # macOS alternative; never installed together
include/preview/preview.hpp
lib/cmake/preview/previewConfig.cmake
lib/cmake/preview/previewTargets.cmake
share/preview/THIRD_PARTY_NOTICES
```

Only the platform `libpreview` shared library is a runtime project binary. There
must be no installed
`libpdfium.so`, `libjpeg.so`, stb library, helper executable, decoder process or
plugin.

The public CMake target exposes only:

- the public include directory;
- the required C++20 compile feature;
- the `PREVIEW_API` import/export definition needed for the shared library.

It must not expose PDFium include paths, stb include paths, GN build directories,
private compile definitions or direct third-party link targets.

## 9. Pinning and update procedure

For every dependency update:

1. Select an immutable upstream revision.
2. Record source URL, revision and archive/file SHA-256.
3. Regenerate the PDFium GN dependency closure.
4. Compare linked components against the previous manifest.
5. Re-audit all licenses/notices.
6. Rebuild the complete PIC static dependency package.
7. Run unit, golden, malformed-corpus and sanitizer tests.
8. Run symbol and dynamic dependency audits.
9. Record performance/memory deltas in an `ai/` review artifact.
10. Update `THIRD_PARTY_NOTICES` and the production manifest atomically.

No dependency update is accepted solely because the build succeeds.
