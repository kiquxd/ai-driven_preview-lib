# preview

`preview` is a small synchronous C++20 library that turns a random-access byte
source into TUI-friendly semantic output. It supports UTF-8/UTF-16 text, bounded
hex dumps, PNG/JPEG/GIF/BMP pixels and metadata, and PDF page rendering.

The library does not print ANSI, own threads, handle input, or know a terminal
protocol. A TUI supplies a viewport, runs `make_preview()` on its own worker,
and renders the owned `TextPreview`, `PixelPreview`, and metadata values.

## Public surface

Consumers include one header and link one target:

```cpp
#include <preview/preview.hpp>

auto engine_result = preview::Engine::create();
if (!engine_result) {
  // engine_result.error() is structured data; the library does not log.
  return;
}
auto engine = std::move(engine_result).value();

auto source_result = preview::open_local_file("document.pdf");
if (!source_result) return;

preview::Request request;
request.viewport.target_pixel_width = 1000;
request.viewport.target_pixel_height = 700;
request.page_index = 0;

auto result = engine.make_preview(*source_result.value(), request);
```

For SSH/SFTP/HTTP integration, implement `preview::ByteSource`. Its `read_at()`
may return short positive reads, must return zero only at EOF, and can translate
transport cancellation or I/O failures into `preview::Error`. No remote or UI
dependency is linked into this library.

`make_preview()` is synchronous and starts no hidden thread. One `Engine` may be
used from multiple caller threads. PDF work is serialized internally because
PDFium's public API is process-global; concurrent use of the same custom
`ByteSource` is the caller's responsibility.

## Build

Supported targets are Linux x86-64 and macOS arm64. The bootstrap pins and
builds a minimal native PDFium closure locally; nothing is installed
system-wide. On macOS, Xcode command line tools and the macOS SDK are required.

```sh
./scripts/bootstrap_dependencies.sh
./scripts/build.sh dev
```

`build.sh` selects `dev` on Linux and `mac-dev` on Apple Silicon. Direct preset
names remain available for CI and debugging.

For an interactive terminal renderer covering text, hex, images, PDF, and
metadata, see [`examples/README.md`](examples/README.md).

Sanitizer build:

```sh
./scripts/build.sh asan
./scripts/build.sh tsan
```

To use another prepared checkout, configure with `PREVIEW_PDFIUM_ROOT` and
`PREVIEW_PDFIUM_OUT`. Normal consumer configure and `find_package(preview)`
never access the network.

Installed usage:

```cmake
find_package(preview CONFIG REQUIRED)
target_link_libraries(my_tui PRIVATE preview::preview)
```

## Formats and behavior

- Text: UTF-8, UTF-8 BOM, UTF-16LE/BE BOM, CRLF/CR normalization, stable source
  byte continuation offsets, deterministic replacement of invalid sequences.
- Hex: bounded rows with offset, bytes, and printable ASCII columns.
- Images: PNG, JPEG, first-frame GIF, and BMP. JPEG EXIF orientation is applied;
  output is bounded and resized without enlargement.
- PDF: one selected page rendered through `FPDF_FILEACCESS`, so a remote source
  does not need to be downloaded as a whole. Missing and wrong passwords are
  separate error codes.
- WebP: intentionally unsupported in 0.1 and reported as structured unsupported
  content in automatic mode.

Zero target pixel dimensions in automatic mode return image/PDF metadata without
guessing terminal geometry. In visual mode they are an invalid request.

## Security and limits

All public operations return `Result<T>` and the exported entry points catch
ordinary C++ exceptions. Input reads, probe/cache size, stb allocations, decoded
raster dimensions, working memory estimates, and output bytes have configurable
limits. Cancellation is cooperative at source reads, JPEG scanlines, transforms,
and PDFium progressive-render pauses. stb has no mid-call cancellation hook.

PDFium runs in-process and does not expose a complete allocator budget. The
library caps its input/cache/output around PDFium, but cannot promise a hard cap
for every parser allocation. Applications previewing hostile PDFs should put the
whole library call in a separately sandboxed process; process isolation is not
part of this single-library MVP.

The 0.1 binary does not promise C++ ABI compatibility across different standard
libraries/toolchains. Rebuild the library on the target platform with the
consumer's compatible C++ toolchain family; Linux `.so` and macOS `.dylib`
artifacts are not interchangeable.

See `THIRD_PARTY_NOTICES` and the installed `share/preview/licenses` directory
for dependency licensing. No AGPL component is used.
