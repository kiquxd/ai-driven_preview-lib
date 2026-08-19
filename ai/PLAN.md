# preview_lib: implementation plan (historical)

> Superseded by the implemented architecture in `README.md` and the current
> dependency inventory in `ai/DEPENDENCIES.md`. PDFium and the dedicated
> libjpeg path were removed; PDFs are delegated to applications.

## 1. Goal

Build a small, embeddable C++20 library that turns a bounded byte source into a
TUI-friendly preview. It must work equally well with local files and remote
files whose bytes arrive through SSH/SFTP.

The repository produces exactly one production binary artifact: the `preview`
library. Required CMake package metadata, headers and license/notice files are
installation metadata, not additional runtime components.
There are no CLI tools, daemons, TUI widgets, adapter libraries, plugins or
application integrations in this project. Test executables are build-only
verification artifacts and are not installed.

The public API is included through one header:

```cpp
#include <preview/preview.hpp>
```

This is an umbrella header, not a requirement to put all implementation into a
single physical file. The library remains normally compiled and linked. A
single-header distribution can be generated later if it becomes useful.

### Required properties

- C++20, Linux-first. Public operations return typed errors and do not
  deliberately throw; third-party/C++ exceptions are caught at the engine
  boundary where recovery is possible.
- Public API depends only on the C++ standard library; implementation
  dependencies are private and statically included in the single library.
- No dependency on a particular TUI, event loop, thread pool, filesystem or SSH
  implementation.
- Library-owned reads, caches, output buffers and pre-decode raster allocations
  are bounded by request limits. Third-party PDFium internal allocations do not
  expose a complete hard memory cap and are treated as an explicit limitation.
  Cancellation is cooperative and latency-bounded where the underlying decoder
  exposes progress points; no hard execution-time guarantee is claimed for a
  single blocking third-party decoder call.
- Synchronous core API with cooperative cancellation; callers decide which
  executor/thread runs it.
- Stable structured output: text, pixels, metadata, unsupported or
  error. The core does not print terminal escape sequences.
- Internally separated format providers, all compiled into the same library.
- Tests are mandatory for every provider and parser.

## 2. Scope

### Version 0.1

- Plain text with UTF-8 validation, BOM handling, line limiting and basic binary
  detection.
- Hex dump fallback for binary/unknown data.
- Content-derived metadata plus source name/MIME hints. Filesystem permissions,
  timestamps and ownership remain the caller's responsibility.
- Image metadata for PNG, JPEG, GIF and BMP using small native parsers
  that read headers only.
- Full image decoding for PNG, JPEG, GIF and BMP.
- PDF metadata/page count and raster preview through PDFium.
- Local file `ByteSource` supplied by the library.
- In-memory and callback-based `ByteSource` for tests and remote integrations.
- Renderer-neutral output suitable for TUI adapters.

### Explicit non-goals for 0.1

- Editable documents.
- Office documents, video decoding and archive browsing.
- Syntax highlighting for every programming language.
- Owning a worker pool, cache database or terminal graphics protocol.
- Silently launching shell commands from the core library.
- Downloading an entire remote file merely to identify or preview it.
- Implementing adapters for the disk analyzer, Faraway or any other application.
- Shipping example applications or additional libraries.
- Stable binary ABI across different compilers or standard-library versions;
  version 0.1 guarantees a small source API, not a cross-toolchain C++ ABI.
- Process sandboxing of image/PDF decoders. The one-library requirement means
  decoders run in the caller's process; version 0.1 must not claim isolation
  from deliberately malicious documents.

## 3. Technical stack and dependency strategy

There is one library and one public target: `preview::preview`. Format handlers
are internal modules, not separately distributed libraries or runtime plugins.
All third-party code is linked privately and must not appear in
`preview/preview.hpp`.

The version 0.1 stack is fixed:

- Language: C++20.
- Build and packaging: CMake with one exported target, `preview::preview`.
- Text, hex, format detection and image metadata: implemented in-tree.
- JPEG decoding: the same bundled `libjpeg-turbo` revision used by PDFium,
  through the libjpeg API and its turbo color-space/scaling extensions.
- PNG, GIF and BMP decoding: vendored `stb_image.h`.
- Image downscaling: vendored `stb_image_resize2.h`.
- PDF parsing and rasterization: PDFium C API.
- Tests: a small in-tree test harness unless a development-only framework is
  explicitly approved; sanitizers use compiler-provided ASan and UBSan.

The canonical dependency inventory, build/runtime distinction and license rules
are in `ai/DEPENDENCIES.md`. If this summary conflicts with that document, stop
and reconcile both before changing the build.

WebP is intentionally out of scope. The library must return the normal
unsupported/hex fallback for WebP and must not link `libwebp`.

### 3.1 Image implementation

Compile the stb implementation in exactly one private translation unit. Configure
`stb_image` with `STBI_NO_STDIO`, disable every unused decoder and disable JPEG so
that JPEG has a single implementation through PDFium's pinned `libjpeg-turbo`.
Initially retain
only PNG, GIF and BMP support.

JPEG flow:

1. Read and validate the header under the request byte budget.
2. Inspect dimensions before allocating output.
3. Select the closest supported IDCT scaling factor to the requested viewport.
4. Decode with `libjpeg-turbo` directly to a four-channel pixel buffer.
5. Perform a final resize with `stb_image_resize2` only when exact viewport
   dimensions are still required.

PNG/GIF/BMP flow:

1. Detect and parse dimensions with the in-tree bounded metadata parser.
2. Reject dimensions and decoded sizes that exceed limits.
3. Decode with `stb_image` from memory; never let it open a path itself.
4. For animated GIF, version 0.1 previews only the first frame and reports the
   animation flag in metadata. Use the ordinary first-image stb path; do not use
   an API that eagerly decodes every animation frame.
5. Downscale once with `stb_image_resize2` into the final output buffer.

The public `PixelPreview` should carry an explicit pixel format. Prefer BGRA8 as
the internal fast path because PDFium can render directly to BGRA and
`libjpeg-turbo` can produce a compatible four-channel layout. Do not perform an
RGBA/BGRA swizzle unless a consumer explicitly requests RGBA.

### 3.2 PDFium implementation

Build a pinned PDFium revision without V8, XFA or Skia and without PDFium sample
programs or tests. The intended feature configuration is:

```text
pdf_enable_v8 = false
pdf_enable_xfa = false
pdf_use_skia = false
pdf_use_agg = true
pdf_use_partition_alloc = false
pdf_enable_brotli = false
pdf_bundle_freetype = true
use_system_libjpeg = false
use_libjpeg_turbo = true
use_system_libopenjpeg2 = false
use_system_libpng = false
use_system_zlib = false
pdf_is_complete_lib = true
is_component_build = false
```

Wrap only the public PDFium C API. Map `ByteSource::read_at()` to
`FPDF_FILEACCESS::m_GetBlock` and place a bounded block cache in front of slow
sources because PDFium may request the same range more than once.

PDF rendering flow:

1. Initialize PDFium once for the lifetime of the library.
2. Open the document with `FPDF_LoadCustomDocument`.
3. Read page count, selected page dimensions and rotation.
4. Calculate target pixel dimensions from `Request::viewport` before allocating.
5. Allocate the final `PixelPreview` BGRA8 buffer.
6. Attach that buffer with `FPDFBitmap_CreateEx`.
7. Fill the requested background and render directly into the final buffer,
   enabling PDFium's limited-image-cache rendering flag where supported.
8. Use the progressive rendering API to poll `std::stop_token` and cancel work.
9. Release page/document/bitmap handles through private RAII wrappers.

PDFium APIs are not thread-safe. All `Engine` instances share one process-global
PDFium lifetime object, initialization state and mutex. Protect every PDFium call
with that mutex in version 0.1; a per-engine mutex is insufficient. Image/text
providers remain independently parallel. Revisit this only if benchmarks
demonstrate that concurrent PDF previews are a real use case.

Disable JavaScript and XFA entirely. Do not execute actions, access external
resources or use shell helpers. Encrypted PDFs without a supplied password return
a typed error rather than falling back to an unbounded parser path.

PDFium does not expose a complete allocator budget for every parser/render path.
Read limits, target bitmap limits and limited image caching are enforced, but a
hard cap on all internal PDFium memory/CPU is impossible in-process. Document
this limitation alongside the lack of a process sandbox.

### 3.3 Linking and licenses

The distributed production artifact is one library. Build PDFium as a complete
position-independent static library with its pinned bundled `libjpeg-turbo`, and
link it privately into `preview`; the standalone JPEG provider uses that same
libjpeg implementation rather than linking a second copy. Compile the stb files
directly into `preview`. Hide all third-party
symbols that are not needed by the public API. Compile with hidden visibility,
mark only public declarations with a small `PREVIEW_API` export macro and use the
linker's archive-symbol exclusion where supported. Test the installed dynamic
symbol table rather than assuming `PRIVATE` in CMake hides symbols.

Pin every dependency to an exact revision and record its revision, source URL,
checksum and license in `third_party/manifest.cmake` (or an equivalently simple
machine-readable manifest). Consumer builds must not download dependencies from
the network implicitly.

Generate and install one `THIRD_PARTY_NOTICES` file containing the required
PDFium/transitive dependency notices, the `libjpeg-turbo` IJG/BSD notices and the
stb dual-license text. These permissive dependencies do not impose copyleft on
consumers, but their attribution and redistribution conditions still apply.

Do not attempt to write a PDF renderer. PDF is a complex executable document
format; a tiny home-grown parser would be fragile and unsafe.

## 4. Public model

Keep the public surface small and value-oriented. `Result<T>` is a concrete,
in-tree C++20 expected-like type declared in the public header; do not leave the
choice unresolved and do not require C++23 `std::expected`.
It must support move-only values such as `Engine`, never require default
construction of `T`, and expose non-throwing status/error access. Accessing the
wrong alternative is a documented programmer precondition, not a runtime throw.

A first API draft:

```cpp
namespace preview {

using Byte = std::byte;

struct Error {
    enum class Code {
        io, invalid_request, invalid_data, unsupported, cancelled,
        limit_exceeded, password_required, wrong_password, backend_failure
    } code;
    std::string message;
};

template <class T> class Result;

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual Result<std::uint64_t> size(
        std::stop_token stop_token) const noexcept = 0;
    virtual Result<std::size_t> read_at(
        std::uint64_t offset,
        std::span<Byte> destination,
        std::stop_token stop_token) const noexcept = 0;
    virtual std::string_view name_hint() const noexcept = 0;
    virtual std::string_view mime_hint() const noexcept = 0;
};

struct Limits {
    std::uint64_t max_probe_bytes = 64 * 1024;
    std::uint64_t max_text_bytes = 2 * 1024 * 1024;
    std::uint64_t max_encoded_image_bytes = 32 * 1024 * 1024;
    std::uint64_t max_pdf_bytes_read = 64 * 1024 * 1024;
    std::uint64_t max_pdf_source_size = 4ull * 1024 * 1024 * 1024;
    std::uint64_t max_total_bytes_read = 64 * 1024 * 1024;
    std::size_t max_input_cache_bytes = 8 * 1024 * 1024;
    std::size_t max_working_bytes = 128 * 1024 * 1024;
    std::size_t max_output_bytes = 16 * 1024 * 1024;
    std::uint64_t max_pixels = 4 * 1024 * 1024;
    std::uint32_t max_pixel_dimension = 4096;
    std::uint32_t max_text_lines = 500;
    std::uint32_t max_line_bytes = 64 * 1024;
};

enum class PixelFormat { bgra8, rgba8 };

struct Viewport {
    std::uint32_t text_columns = 0;
    std::uint32_t text_rows = 0;
    std::uint32_t target_pixel_width = 0;
    std::uint32_t target_pixel_height = 0;
};

enum class Mode { automatic, visual, metadata, hex };

struct Request {
    Viewport viewport;
    Limits limits;
    Mode mode = Mode::automatic;
    std::uint64_t byte_offset = 0;
    std::uint32_t page_index = 0;
    std::uint32_t background_rgba = 0xffffffff;
    PixelFormat pixel_format = PixelFormat::bgra8;
    std::string_view pdf_password;
    std::stop_token stop_token;
};

struct TextLine { std::string text; /* optional format-neutral spans later */ };
struct TextPreview {
    std::vector<TextLine> lines;
    std::uint64_t source_begin;
    std::uint64_t source_end;
    std::uint64_t next_offset;
    bool has_more;
};
struct PixelPreview {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    PixelFormat format;
    std::vector<Byte> pixels;
};
struct MetadataItem { std::string key; std::string value; };
struct Metadata { std::vector<MetadataItem> items; };
struct UnsupportedContent { std::string reason; };
struct Warning { std::string code; std::string message; };

using Content = std::variant<
    std::monostate,
    TextPreview,
    PixelPreview,
    UnsupportedContent>;

struct Preview {
    std::string detected_mime;
    std::string detected_format;
    Content content;
    Metadata metadata;
    std::vector<Warning> warnings;
    bool truncated = false;
};

class Engine {
public:
    static Result<Engine> create() noexcept;
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Result<Preview> make_preview(
        const ByteSource&,
        const Request&) const noexcept;

private:
    struct Impl;
    explicit Engine(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

Result<std::unique_ptr<ByteSource>> open_local_file(
    const std::filesystem::path&) noexcept;

} // namespace preview
```

The exact ownership types may change during implementation, but preserve these
properties:

- `ByteSource` supports random range reads and does not expose file descriptors.
- `read_at()` may return a short positive read. It returns zero only at EOF,
  reports transport failures as `Error::io`, and checks its stop token before
  and during a blocking operation where its transport permits cancellation.
- `name_hint()` and `mime_hint()` views remain valid for the lifetime of the
  source. They are untrusted byte-string hints, never detection authority. ASCII
  extension matching is byte-based; invalid UTF-8 is escaped/replaced before a
  hint is copied into human-readable metadata.
- `size()` observes cancellation and returns the size captured for this source.
  `pdf_password` is borrowed only for the synchronous call, is never retained or
  logged, and wrong/missing passwords have distinct error codes for TUI prompts.
- A source may be slow, remote and non-thread-safe; do not assume otherwise.
- Concurrent calls may use one `Engine`, but concurrent calls using the same
  non-thread-safe source are the caller's responsibility. PDF work is internally
  serialized because PDFium is not thread-safe.
- `Engine` is immutable while rendering. Its built-in format handlers are fixed.
- One preview call reads a source sequentially from the caller's perspective;
  providers do not start hidden threads.
- `Preview` owns its data, so it remains valid after the source/request dies.
- Metadata accompanies visual/text content instead of competing with it in one
  variant. `Mode::metadata` returns `std::monostate` content plus populated
  metadata; `UnsupportedContent` is reserved for a recognized format without a
  renderer.
- `byte_offset` is a source-byte continuation position for text/hex. Providers
  report the consumed range and `next_offset`; the application does not infer it
  from UTF-8 display strings. PDF scrolling uses `page_index` instead.
- A caller-supplied text offset that lands inside a UTF code unit is advanced to
  the next valid boundary with a warning. Offsets returned by the provider are
  always safe to feed into the next request. A TUI keeps prior returned offsets
  itself if it wants backward scrolling.
- Text and hex do not reject a source merely because its total size is large;
  they are bounded by bytes actually read. Encoded images and PDF use their own
  source-size/read limits.
- Errors are data, not logging side effects.
- `detected_mime`, `detected_format`, metadata keys and warning codes are stable,
  lowercase, locale-neutral identifiers. Human messages/values are UTF-8. The
  library performs no localization.
- `Mode::automatic` selects text or visual content, `Mode::visual` requires a
  decoded visual result for image/PDF, `Mode::metadata` omits body content, and
  `Mode::hex` bypasses normal format rendering. Tests lock down these semantics.
- In automatic/metadata mode, zero target pixel dimensions produce metadata with
  empty content for images/PDF. In visual mode they produce `invalid_request`;
  the library never guesses terminal pixel geometry.
- `text_rows` requests the number of returned lines and is capped by limits;
  zero means use the configured line limit. `text_columns` controls hex layout
  only in 0.1; ordinary text remains unclipped semantic UTF-8. `pixel_format`
  makes any BGRA/RGBA conversion an explicit consumer choice.

## 5. Remote-source contract

The library must not know about SSH. A consumer can implement `ByteSource` over
any range-capable transport. The required source behavior is deliberately small:

- return a stable size for the lifetime of one preview call;
- implement the documented partial-read/EOF rules;
- propagate transport failures and cancellation;
- keep hint views alive for the source lifetime.

The engine, not the source, owns per-request byte accounting. It wraps the source
in an internal budgeted reader and shares a bounded probe/block cache among
providers. A remote source may additionally coalesce/cache reads for latency, but
correctness must not depend on it.

The contract is tested with an in-memory simulated slow source that performs
short reads, repeats ranges, blocks until cancelled and injects transport errors.
No SSH implementation or remote identity/cache invalidation policy belongs in
this repository.

## 6. Format detection and provider selection

Detection order:

1. Magic bytes from a shared probe buffer.
2. Lightweight content inspection for text/binary classification.
3. Application-supplied MIME hint as an untrusted tie-breaker.
4. Filename extension as another weak tie-breaker.
5. Hex fallback.

`Engine` reads one bounded prefix and shares it through `ProbeContext`; providers
must not independently reread the same header. `ProbeResult` includes confidence,
required capability and estimated cost. Choose the highest-confidence compatible
provider, with deterministic tie-breaking.

Never trust MIME or an extension enough to pass malformed bytes unchecked to a
decoder. A known-but-disabled format such as WebP returns unsupported content in
automatic mode; callers can explicitly request `Mode::hex`.

## 7. Providers

### Text provider

- Detect UTF-8, UTF-8 BOM, UTF-16 LE/BE BOM and plain ASCII.
- Normalize CRLF/CR for display without modifying original offsets.
- Replace invalid sequences deterministically and expose a warning flag.
- Preserve tabs in semantic output; terminal-specific expansion belongs to the
  consumer.
- Bound input and output by bytes, code points and line count, but do not make
  terminal-specific clipping decisions.
- Bound line length to protect the TUI from minified/hostile files.
- Support byte-offset windows and return exact continuation offsets.
- Keep syntax highlighting as an optional postprocessor, not part of decoding.

Do not implement `wcwidth`, emoji width or styled syntax spans in 0.1. The TUI
already knows its own width rules and must clip the returned UTF-8 lines.

### Hex provider

- Fixed-size bounded reads.
- Offset, hex bytes and printable ASCII columns.
- Configurable bytes per row based on viewport width.
- Works as the final fallback for every readable source.

### Image metadata provider

- Parse only enough structure to obtain format, dimensions, color information
  and animation/frame hints.
- Validate arithmetic before allocation.
- Stop after the needed chunks/markers.
- Never decompress pixels.

### Image pixel provider

- Decode directly to BGRA8 where the decoder supports it; otherwise convert once.
- Parse JPEG EXIF orientation with a bounded in-tree parser and apply it before
  the final resize. Other orientation metadata is out of scope for 0.1.
- Downscale during/just after decoding; never retain multiple full-size copies.
- Preserve aspect ratio and enforce pixel/allocation limits before decoding.
- The library returns pixels; consumers choose block characters, Kitty, Sixel
  or another terminal renderer outside this repository.

### PDF provider

- Isolated behind the internal provider boundary; PDFium types remain private.
- Render only the selected page directly at target resolution.
- Enforce page, pixel and byte budgets and cooperative cancellation.
- Do not execute embedded actions, open links or access external resources.
- Return metadata separately when pixel rendering is unavailable.

## 8. Performance rules

- No whole-file reads except encoded image formats that require an in-memory
  decoder, and only when source size is within `max_encoded_image_bytes`.
- Probe buffer defaults to 64 KiB and is reused across providers.
- Text preview should usually complete after one bounded prefix read.
- Avoid copies by accepting spans internally and moving final output.
- Use checked integer arithmetic for offsets, dimensions and buffer sizes.
- Resize images to requested output dimensions, not terminal-independent maxima.
- Do not add a global result cache to the core. Applications cache completed
  `Preview` objects using their own file identity.
- Keep performance regression cases inside the test suite; do not ship a
  separate benchmark program in version 0.1.

Initial measurable targets on a development machine:

- Text/hex preview of a local file: no more than 2 MiB read by default.
- Header-only image metadata: no more than 256 KiB read for supported formats.
- Peak owned output memory bounded by `max_output_bytes` plus a documented and
  pre-validated decoder working set capped by `max_working_bytes` where the
  decoder permits control. PNG/GIF/BMP may temporarily require the decoded
  source raster plus the final resized raster; reject them before decode when
  that estimate exceeds the working limit.
- `max_working_bytes` includes encoded input buffers, intermediate rasters and
  resize scratch owned by the library; `max_output_bytes` covers the final
  `Preview`. PDFium internal allocations are the documented exception.
- Cancellation checked between reads and expensive transform stages.

`libjpeg-turbo` scanline/phase boundaries and PDFium progressive rendering check
cancellation. `stb_image` has no general mid-call cancellation hook, so PNG/GIF/
BMP cancellation is best-effort before and after the decode call. Do not promise
a fixed latency for remote sources or any single blocking decoder call.

## 9. TUI integration boundary

The library returns semantic output. Consumers convert it to their widgets:

```text
TextPreview     -> styled rows / scrollable text widget
PixelPreview    -> terminal image protocol or colored cell rasterizer
Preview.metadata -> optional two-column side panel/status rows
UnsupportedContent -> message plus explicit `Mode::hex` action
```

The library must not emit ANSI sequences, query terminal capabilities, handle
keys or own scroll state. The TUI supplies both cell dimensions and, when it can
display pixels, an explicit target pixel size. If target pixel dimensions are
zero, image/PDF automatic mode returns metadata/unsupported content rather than
guessing terminal cell aspect ratio. No TUI adapter is implemented here.

For async integration, the caller submits synchronous `make_preview()` work to
its executor, owns the `ByteSource` until completion, cancels the associated
`stop_source` when selection changes and discards stale results using its own
request generation ID. The library does not embed UI request IDs or callbacks.

## 10. Repository layout

```text
preview_lib/
├── CMakeLists.txt
├── cmake/
├── include/preview/
│   └── preview.hpp              # only required public include
├── src/
│   ├── engine.cpp
│   ├── detection.cpp
│   ├── local_file_source.cpp
│   ├── text_provider.cpp
│   ├── hex_provider.cpp
│   ├── image_metadata_provider.cpp
│   └── detail/
├── third_party/                  # only sources linked into preview
├── src/providers/
│   ├── image_decoder/
│   └── pdf/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── corpus/
│   └── fuzz/
├── LICENSE
├── README.md
└── PLAN.md
```

Only declarations intended for consumers belong in `preview.hpp`. Implementation
details must not leak through public types. `Engine` uses PImpl to hide PDFium
and provider state, but the project does not claim ABI compatibility across
different C++ toolchains or standard libraries.

## 11. Build and packaging

Provide exactly one installable CMake target:

```text
preview::preview
```

Version 0.1 builds `preview` as one shared library. A complete PDFium PIC static
archive, including its one pinned `libjpeg-turbo` implementation, is folded into
it; stb code is compiled directly into it. This avoids shipping additional renderer `.so`
files and makes the "one production library" requirement testable. Static-only
consumer packaging is outside 0.1 because correctly merging PDFium's archives
and transitive objects requires a separate, explicit design.

Consumer usage should remain:

```cmake
find_package(preview CONFIG REQUIRED)
target_link_libraries(app PRIVATE preview::preview)
```

and:

```cpp
#include <preview/preview.hpp>
```

Required options:

```text
PREVIEW_BUILD_TESTS
PREVIEW_ENABLE_SANITIZERS
```

Dependency acquisition is separate from an ordinary consumer configure. CMake
accepts a pinned prepared `PREVIEW_PDFIUM_ROOT`; it must contain PDFium public
headers, the libjpeg headers used by that build, PIC static archives and license
metadata matching the manifest. The repository may provide developer
bootstrap instructions/scripts, but `find_package(preview)` and
`add_subdirectory` never download or update dependencies.

Export/install package config and support `add_subdirectory`. Do not fetch
dependencies implicitly during a consumer build. Internal providers and vendored
code are always linked into the same `preview` target; never export another
project target. Tests are created only under `PREVIEW_BUILD_TESTS` and are never
installed.

## 12. Test plan

Use a lightweight test setup. If avoiding a test-framework dependency, implement
a tiny in-tree registration/assertion harness; do not reduce test coverage merely
to avoid one development-only dependency.

### Unit tests

- Short reads, EOF, transport error and cancellation in `ByteSource`.
- Stable captured size and local-file unexpected-EOF/change behavior.
- Engine-side byte budget enforcement independent of source behavior.
- `byte_offset`, consumed ranges, `next_offset` and `has_more` semantics.
- Detection precedence and ambiguous/misleading extensions.
- UTF encodings, BOMs, invalid sequences, CRLF and huge lines.
- Text clipping and byte/read budgets.
- Huge/sparse text and binary sources preview correctly without whole-source
  rejection or size-dependent allocation.
- Exact hex formatting and viewport edge cases.
- Valid and truncated headers for every image metadata parser.
- Overflowing/malicious dimensions.
- Output, input-cache and decoder working-memory limit calculations.
- Provider ranking and deterministic fallback.
- Metadata accompanying text/pixels and all `Mode` values.
- Every error code and limit path.
- Missing/wrong/correct PDF passwords, with an assertion that passwords never
  appear in errors, warnings or logs.

### Integration tests

- Local files through `LocalFileSource`.
- Callback source that deliberately returns tiny partial reads.
- Slow simulated remote source with cancellation.
- Repeated-range PDF source verifies bounded block caching and sticky callback
  error propagation from `FPDF_FILEACCESS`.
- Concurrent text/image calls and serialized PDFium calls under ThreadSanitizer
  when supported by the toolchain.
- The single library links only the pinned, approved dependency set.
- `ldd`/symbol-table checks verify that no unexpected runtime dependency or
  public third-party symbol leaks from the installed library.
- Consumer compilation test that includes only `preview/preview.hpp`.
- Installation test using `find_package(preview)` from a separate mini-project.

### Golden tests

- Stable text and hex output.
- Small decoded image fixtures compared by dimensions and pixel hash.
- PDF page renders compared with tolerant perceptual/error metrics rather than
  byte-identical output across backend versions.

### Robustness tests

- Fuzz detection, UTF decoding and each native metadata parser.
- Run unit/corpus tests under ASan and UBSan.
- Add regression fixtures for every discovered crash or incorrect parse.
- Corpus must contain truncated files, invalid lengths, decompression bombs as
  metadata-only fixtures, enormous declared dimensions and misleading suffixes.

Test fixtures should be tiny and either authored in-tree or have documented
redistribution rights.

Third-party parsers operate in-process, so robustness testing reduces but cannot
replace process isolation. Document this security boundary in README and keep
dependency revisions easy to update when upstream security fixes are released.

## 13. Implementation milestones

The MVP is the completion of milestones 0 through 5 below. Each milestone must
leave the same single library target buildable and its tests green. The agent
must not add SSH, TUI rendering, caching services, syntax highlighting, extra
formats or executables to make a milestone look more complete.

Implementation order is intentional:

```text
prove dependency packaging
    -> freeze source/request/result contracts
    -> deliver text + hex fallback
    -> deliver bounded image metadata + pixels
    -> deliver PDFium range rendering
    -> harden/install the one library
```

After milestone 1, public API changes require updating the consumer compile test
and documenting the reason. After milestone 2, every later provider must reuse
the same detection, budget, cancellation, metadata and error paths rather than
creating format-specific public APIs.

### Milestone 0: dependency and packaging spike

- Select and record an exact PDFium revision (including its pinned
  `libjpeg-turbo` transitive revision) plus exact revisions/checksums for both stb
  headers.
- Produce or locate PIC static dependency builds with the required licenses.
- Link a throwaway compile-only test target against them during development and
  prove that the final shared-library link can hide third-party symbols.
- Verify the selected PDFium configuration has V8, XFA and Skia disabled and can
  call `FPDF_LoadCustomDocument`, `FPDFBitmap_CreateEx` and progressive render.
- Verify the standalone JPEG provider can call the libjpeg API from the same
  complete dependency closure without linking a second libjpeg archive.
- Write `third_party/manifest.cmake` and initial `THIRD_PARTY_NOTICES`.

The throwaway target is test-only and never installed. Do not start broad library
implementation until this spike proves that the one-library packaging constraint
is achievable on the target Linux toolchain.

Exit criteria: a minimal `libpreview.so` link succeeds with the pinned static
dependencies, exported symbols are controllable, and all dependency notices are
accounted for.

### Milestone 1: skeleton and contracts

- CMake project, public umbrella header and error/result types.
- `ByteSource`, memory test source, callback test source and public local file
  source, including stop-token and partial-read semantics.
- Limits/budget tracker and cancellation checks.
- Concrete `Preview` envelope, metadata, mode, viewport and continuation model.
- Minimal test runner and CI-friendly test command.
- Consumer compile/install test.

Exit criteria: a separate program can include the single public header, open a
local source and receive a structured unsupported result. A compile-only mock
remote source can implement the complete interface without including any private
header.

### Milestone 2: text and hex

- Shared probe buffer and deterministic provider selection.
- UTF-aware text provider.
- Hex fallback.
- Unit, golden, sanitizer and initial fuzz tests.

Exit criteria: arbitrary files always yield text, hex or a typed I/O error while
respecting configured read/output limits.

### Milestone 3: image metadata

- PNG/JPEG/GIF/BMP header parsers.
- Bounded JPEG EXIF-orientation parser.
- Checked arithmetic and malformed corpus.
- Metadata output and detection tests.

Exit criteria: dimensions/type are reported without whole-file reads and malformed
inputs do not crash or allocate based on unchecked dimensions.

### Milestone 4: image pixels

- `libjpeg-turbo` JPEG decoder with scaled decode.
- `stb_image` PNG/GIF/BMP decoder with all unused formats disabled.
- `stb_image_resize2` resizing and orientation/BGRA conversion.
- Pixel-output contract and peak-memory performance checks in tests.

Exit criteria: the single library returns bounded BGRA previews for JPEG, PNG,
GIF and BMP, rejects WebP as unsupported, and passes malformed-image and memory-
limit tests.

### Milestone 5: PDF support and release hardening

- Lean, pinned PDFium build linked privately into the same library target.
- `ByteSource`/`FPDF_FILEACCESS` bridge and bounded block cache.
- Direct rendering into the final BGRA buffer with progressive cancellation.
- Target-size page rendering and metadata.
- Malformed/encrypted PDF behavior.
- Cancellation and resource-limit tests.
- Limited-image-cache flag and documented PDFium hard-memory-cap limitation.
- Global PDFium lifetime and mutex behavior tests.
- Missing/wrong/correct password behavior and sticky errors from the C callback.
- Installed `THIRD_PARTY_NOTICES` and dependency manifest.

Exit criteria: PDF rendering adds no public-header dependency, no PDFium symbol
is exported, licensing notices are packaged, and malformed/cancelled renders do
not leak PDFium resources.

### MVP final verification

Run, in order:

1. Clean configure/build with only documented dependency roots.
2. Unit, integration, golden and malformed-corpus tests.
3. ASan and UBSan builds; TSan for library-owned concurrency where available.
4. Consumer install test using only `preview/preview.hpp` and
   `preview::preview`.
5. Dynamic symbol and runtime dependency audit of installed `libpreview.so`.
6. A read-count test proving text/image metadata use bounded prefixes and PDFium
   repeated ranges hit the bounded cache.
7. Cancellation tests for a blocked source, JPEG scanline decode and PDFium
   progressive render; explicitly record stb's mid-call limitation.
8. Peak-memory/performance runs for representative large JPEG, PNG, first-frame
   GIF, BMP and PDF fixtures.

The MVP is done only when a hypothetical TUI can open a local source or provide
its own remote `ByteSource`, submit one synchronous request on a worker, cancel
it, receive owned semantic output and render it without including a private or
third-party header.

## 14. Rules for the coding agent

- Implement milestones in order; do not begin PDF before text/hex and source
  limits are tested.
- Before each milestone, write or update its acceptance tests.
- Do not introduce a dependency without documenting why the format cannot be
  supported safely and correctly without it.
- Do not add another production target, executable, plugin, application adapter
  or public header entry point.
- Do not expose third-party types from `preview.hpp`.
- Do not call `system()`, parse shell-built command strings or perform network
  access in the library.
- Treat every input as untrusted, including local files.
- Never let C++ exceptions cross PDFium/libjpeg/stb C callbacks; translate them
  to sticky typed errors after control returns from the C API.
- All size/offset multiplication and addition must be checked.
- Preserve deterministic output for identical bytes and request options.
- Keep public API changes deliberate; add a small compile-only API test for every
  supported usage pattern.
- Run unit/integration tests and sanitizer tests before declaring a milestone
  complete.

## 15. Definition of done for 0.1

- One public include is sufficient for all consumer-visible API.
- Exactly one production library target is built and installed.
- PDFium, its single bundled `libjpeg-turbo`, `stb_image` and
  `stb_image_resize2` are pinned private implementation dependencies; no second
  JPEG implementation or other image/PDF dependency is introduced.
- Text, hex and listed image metadata formats work under explicit budgets.
- JPEG, PNG, GIF and BMP pixel previews and PDF page previews work under explicit
  budgets; WebP is unsupported and never linked.
- Local, memory and callback/range sources are covered by tests.
- No TUI, SSH or external decoder type leaks into the public API.
- Unit, integration, golden and malformed-corpus tests pass under normal, ASan
  and UBSan builds.
- CMake install/export and `add_subdirectory` consumption are tested.
- README contains only library usage snippets; it does not implement application
  integrations.
- `THIRD_PARTY_NOTICES` and the exact dependency manifest are installed with the
  library.
- Performance tests report bytes read, allocations/peak output memory and elapsed
  time for representative text, binary, image and PDF previews.
