# Dependency inventory

This document describes the implemented MVP, not the superseded PDFium design.

## Runtime/library dependency

`preview_lib` has no required third-party dynamic runtime dependency. It uses the
C++ standard library and operating-system file APIs.

## Vendored source dependency

- Project: [stb](https://github.com/nothings/stb)
- Pinned revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`
- Files: `stb_image.h`, `stb_image_resize2.h`
- Purpose: PNG/JPEG/GIF/BMP decoding and raster resizing
- License: MIT or public domain, at the user's choice
- License file: `third_party/stb/LICENSE`
- Integrity hashes and revision: `third_party/manifest.cmake`

stb is compiled privately into the shared library. Its headers and symbols are
not part of the public API. WebP is disabled. JPEG uses stb as well, so no system
libjpeg or duplicate JPEG implementation is linked.

## Build tools

- CMake 3.24 or newer
- a C++20 compiler and Make
- Apple Command Line Tools on macOS arm64

The bootstrap script only validates vendored headers. It does not download or
install anything. PDFium, depot_tools, GN, Ninja, Python, Fontconfig, full Xcode,
MuPDF, Poppler, libjpeg, and AGPL/GPL libraries are not dependencies.
