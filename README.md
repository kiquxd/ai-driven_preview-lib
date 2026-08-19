# preview

`preview` is a synchronous C++20 library that converts a random-access byte
source into UI-independent preview data. It produces decoded text with semantic
syntax spans, bounded hex dumps, image pixels and metadata. PDF files are
detected but intentionally delegated to the application's system viewer.

The library does not print ANSI, choose colors, own threads, handle input, or
start external programs. That makes the same backend suitable for FTXUI,
ncurses, GUI, CLI, local files, and remote range-readable sources.

## Public API

Consumers include one public header and link one CMake target:

```cpp
#include <preview/preview.hpp>

auto engine_result = preview::Engine::create();
if (!engine_result) return;
auto engine = std::move(engine_result).value();

auto source_result = preview::open_local_file("main.cpp");
if (!source_result) return;

preview::Request request;
request.viewport.text_columns = 100;
request.viewport.text_rows = 30;
request.viewport.target_pixel_width = 100;
request.viewport.target_pixel_height = 60;

auto result = engine.make_preview(*source_result.value(), request);
```

For SSH/SFTP/HTTP integration, implement `preview::ByteSource`. Its
`read_at()` may return short positive reads and must return zero only at EOF.
All returned preview data owns its storage; request strings and the source only
need to remain valid for the synchronous call.

`TextLine::styles` contains sorted, non-overlapping UTF-8 byte ranges and
semantic tokens. The consumer chooses colors and typography. Basic lexical
highlighting is available for C/C++, Python, Bash, JSON, CMake, and Markdown.
Detection uses an explicit language hint first, then filename/extension, MIME,
and shebang. Set `syntax_highlighting = false` to skip it.

## Supported systems and build

Only Linux x86-64 and macOS arm64 are supported. A C++20 compiler, CMake 3.24+
and Make are required. On macOS, Apple Command Line Tools are sufficient; a full
Xcode installation is not required.

The only third-party code is the pinned, vendored header-only stb image decoder
and resizer. Bootstrap validates it and performs no network download.

```sh
./scripts/bootstrap_dependencies.sh
./scripts/build.sh dev
```

Sanitizer variants are `./scripts/build.sh asan` and
`./scripts/build.sh tsan`. To build the interactive example, see
[`examples/README.md`](examples/README.md).

Install and consume through CMake:

```sh
cmake --install build/dev --prefix /your/prefix
```

```cmake
find_package(preview CONFIG REQUIRED)
target_link_libraries(my_tui PRIVATE preview::preview)
```

The installation contains `preview/preview.hpp`, the shared library, CMake
package files, and license notices. It does not contain helper processes or
runtime decoder libraries.

## Formats and behavior

- Text: UTF-8, UTF-8 BOM and UTF-16LE/BE BOM; normalized line endings;
  deterministic replacement of invalid sequences; continuation offsets.
- Syntax: basic dependency-free lexical spans for C/C++, Python, Bash, JSON,
  CMake, and Markdown. It is intentionally not a compiler-grade parser.
- Hex: bounded rows containing offset, byte values, and printable ASCII.
- Images: PNG, JPEG, first-frame GIF, and BMP through stb. JPEG EXIF orientation
  is applied. The result is resized to the requested pixel viewport without
  enlargement.
- PDF: detected as `application/pdf` and returned as `UnsupportedContent` with
  an instruction to open it externally. The library neither renders nor starts
  a viewer.
- WebP: intentionally unsupported.

For sharp TUI images, request the actual pane dimensions and render one terminal
cell as two vertical pixels. A narrow sidebar necessarily loses detail; request
a second, larger preview for fullscreen display instead of scaling the small
raster afterwards.

## Limits, cancellation, and threading

Probe/input/output sizes, decoded dimensions, memory estimates, text lines, and
syntax span counts are bounded by `Request::limits`. Cancellation is cooperative
through `std::stop_token` at source reads and provider processing boundaries.
stb has no mid-decode cancellation hook, so encoded size and working-memory
limits are checked before decoding.

`make_preview()` starts no hidden thread and may be called concurrently on one
`Engine` with distinct sources. Concurrent access to the same custom
`ByteSource` is the caller's responsibility. The binary has a C++ API/ABI, so
build it with a toolchain compatible with the consumer; Linux and macOS
artifacts are not interchangeable.

No AGPL component is used. See `THIRD_PARTY_NOTICES` and
`share/preview/licenses` in an installation.

## Architecture and processing pipeline

The public boundary is deliberately small:

```text
ByteSource + Request
        │
        ▼
 bounded probe ──► format/text detection
        │
        ▼
 Engine dispatch
   ├─ text decode ─► syntax lexer ─► TextPreview
   ├─ hex formatter ───────────────► TextPreview
   ├─ stb decode/resize ───────────► PixelPreview
   └─ unsupported policy ──────────► UnsupportedContent
        │
        ▼
 Preview { content, metadata, warnings }
```

`detection.cpp` performs bounded signature and text probing. `engine.cpp`
validates the request and selects a provider. `text_provider.cpp` decodes and
paginates text, then `syntax_highlighter.cpp` adds semantic ranges.
`hex_provider.cpp` formats binary data. `image_provider.cpp` validates image
limits, decodes, applies orientation, and resizes; the stb implementation is
isolated in `stb_impl.cpp`. `io.cpp` centralizes bounded reads, while
`local_file_source.cpp` is only one concrete `ByteSource` adapter.

Providers return semantic data rather than terminal markup. Scheduling,
caching between requests, keyboard actions, colors, fullscreen behavior, and
launching a PDF viewer remain application responsibilities. This separation is
what lets a TUI integrate the library without inheriting its rendering policy.
