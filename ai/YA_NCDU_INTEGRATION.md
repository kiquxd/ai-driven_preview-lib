# ya-ncdu integration

The preview MVP is integrated into a local checkout at
`/home/kiquxd/ya-ncdu`.

## Boundary

- `PreviewService` owns one `std::jthread`, a latest-only pending request,
  cooperative cancellation, and generation-based stale-result rejection.
- The worker calls only the public `<preview/preview.hpp>` API and publishes an
  immutable snapshot.
- `RenderPreview` converts text, metadata, unsupported results, and RGBA pixels
  into FTXUI DOM elements. It performs no filesystem I/O or decoding.
- Bitmap output uses `▀`: foreground is the upper pixel and background is the
  lower pixel, preserving two vertical pixels per terminal cell.

## Dependency integration

`ya-ncdu` adds `third_party/preview_lib`, pinned as a Git submodule, with
`add_subdirectory` and links the real `preview::preview` target, so CMake
selects `.so` or `.dylib` without a hard-coded filename. `PREVIEW_ROOT` can
still point at a separate checkout while developing the library.
FTXUI is locally bootstrapped at pinned revision
`c100eab535db2283b78d30fcb6d082a1f84fb683` (v7.0.1). Chafa and libmagic were
removed from the application dependency set because their roles are covered by
`preview_lib` and the direct FTXUI pixel adapter.

## Verification

- Full CMake build succeeds without warnings.
- `preview_integration_test` covers text, latest-request-wins cancellation,
  PNG pixels rendered into an FTXUI screen, PDF rendering, and directory safety.
- Manual PTY run against `preview_lib/tests/corpus` displayed JPEG and PDF
  previews during navigation and exited cleanly with `q`.
- The configure wrapper selects the PDFium Clang/Ninja toolchain on Linux
  x86-64 and macOS arm64, and refreshes CMake caches copied between machines.

## Known MVP limitations

- The preview viewport is estimated from terminal dimensions and the current
  fixed-width directory columns; a future responsive layout should feed exact
  measured pane dimensions back to the service.
- PDF password entry and page navigation are not yet exposed by ya-ncdu.
- There is no preview result cache in ya-ncdu; cancellation and debounce prevent
  stale work, but revisiting a file decodes it again.
