# MVP implementation handoff

Status date: 2026-08-19.

## Delivered

- One production shared library (`libpreview.so` on Linux,
  `libpreview.dylib` on macOS) and one exported CMake target,
  `preview::preview`.
- One required public include, `preview/preview.hpp`; PDFium, libjpeg and stb
  types do not cross the public boundary.
- Local-file and custom random-access `ByteSource` support.
- Text, hex, PNG, JPEG, GIF, BMP, WebP-disabled, and PDF provider paths.
- Shared probe reuse, PDF 64 KiB range cache, total-read budgets, stop-token
  cancellation, output/working limits, and thread-local bounded stb allocator.
- PDFium global lifetime/mutex, progressive render cancellation, and distinct
  missing/wrong password errors.
- Install/export package, versioned symbol allowlist, dependency manifest, full
  copied license texts, fixtures, unit/integration/malformed tests, and external
  installed-consumer compile/run check.

Pinned revisions are recorded in `third_party/manifest.cmake`.

## Verification performed

- `cmake --preset dev`, build, and CTest: passed.
- `cmake --preset asan`, build, and CTest with ASan+UBSan+LeakSanitizer: passed.
- `cmake --preset tsan`, build, and CTest with ThreadSanitizer: passed.
- Real decode/render fixtures: PNG, JPEG, GIF, BMP, unencrypted PDF, encrypted
  PDF with missing/wrong/correct UTF-8 password: passed.
- Partial-read source, cancellation, UTF-16, invalid UTF-8 replacement,
  malformed PNG, total-read limit, probe reuse, and PDF probe-cache tests:
  passed.
- Deterministic truncation/byte-mutation corpus over every image format and PDF,
  including sanitizer runs: passed.
- Installed package consumed by a separate CMake project using only
  `find_package(preview)`, `preview::preview`, and `preview/preview.hpp`: passed.
- A separate `add_subdirectory` consumer with tests disabled: passed.
- Dynamic symbol audit: only the intended `preview::Engine` operations and
  `preview::open_local_file` are exported; no PDFium/libjpeg/stb symbol is
  exported.
- Reference clean Release `libpreview.so.0.1.0` size: approximately 12 MiB.

## Problems found and resolutions

1. `depot_tools` failed when invoked by a relative path because its CIPD scripts
   locate sibling files through `PATH`. The bootstrap now always prepends the
   absolute depot_tools directory.
2. The host lacked `pkg-config` and development linker symlinks. A pinned
   `pkgconf` is built under `.deps/tools`; PDFium's Clang, sysroot, Ninja, and LLD
   are reused by the presets. Nothing is installed globally.
3. Locally prefixed pkgconf initially emitted sysroot `/usr/include` twice and
   broke libstdc++ `#include_next`. It is configured with explicit system include
   and library directories.
4. PDFium's libjpeg API symbols are Chromium-renamed. The standalone JPEG
   provider includes `jpeglibmangler.h` and calls the one bundled implementation;
   no duplicate libjpeg is linked.
5. `libjpeg` error handling uses `longjmp`. Decoder-owned pixel memory is kept in
   a C allocation across those calls, so no live C++ object destructor can be
   skipped by a decoder failure.
6. The first shared-library link exported weak standard-library symbols. Hidden
   visibility plus `cmake/preview.map` now enforces the intended dynamic-symbol
   allowlist.
7. An instrumented shared library does not itself carry Clang's complete static
   ASan runtime. The sanitizer configuration permits its hooks to remain
   unresolved until the instrumented test executable supplies the runtime;
   normal builds retain `--no-undefined`.

## Platform support update

- Supported platform matrix: Linux x86-64 and macOS arm64.
- PDFium uses separate `out/Preview-linux-x64` and
  `out/Preview-mac-arm64` GN directories.
- macOS uses the system Xcode SDK, an arm64 target, AppKit, CoreFoundation,
  CoreGraphics, and Mach-O `-force_load` instead of Linux Fontconfig and ELF
  whole-archive/version-script flags.
- `scripts/build.sh` selects the native preset and refreshes relocated or legacy
  CMake caches.
- Linux is executed in this workspace. The macOS branch is configuration- and
  source-audited but must still be compiled and run on an Apple Silicon host.

## Known limitations, not silent TODOs

- No Windows, Linux arm64, macOS x86-64, or universal/fat Mach-O build is
  supported.
- GIF renders the first frame only; WebP is deliberately absent.
- stb cancellation is checked before and after a decode, not during the single
  decoder call.
- PDFium internal allocations cannot be placed under a complete hard memory
  cap. Hostile-document isolation requires an application-owned sandbox process.
- PDF calls are globally serialized. This favors correctness and predictable
  integration over parallel page throughput in 0.1.
- The current tests are a compact custom runner, not a fuzzing campaign. A
  persistent malformed corpus and libFuzzer jobs remain release-hardening work.
