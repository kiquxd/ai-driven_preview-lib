# macOS arm64 support

Status date: 2026-08-19.

## Supported matrix

- Linux x86-64: built and tested in the development workspace.
- macOS arm64, deployment target 13.0: implemented; native verification is
  required on an Apple Silicon Mac.

Other OS/architecture combinations fail early with a clear bootstrap or CMake
error. A universal/fat Mach-O binary is intentionally out of scope.

## PDFium

The pinned PDFium revision is unchanged. The bootstrap selects a native output:

- `out/Preview-linux-x64`, `target_os="linux"`, `target_cpu="x64"`, sysroot on;
- `out/Preview-mac-arm64`, `target_os="mac"`, `target_cpu="arm64"`, system Xcode
  and SDK, sysroot off.

Both produce the complete static `obj/libpdfium.a`. macOS folds it into
`libpreview.dylib` with `-force_load` and links the framework closure declared by
the selected PDFium source graph: AppKit, CoreFoundation, and CoreGraphics.
Linux retains Fontconfig, LLD, the version script, and ELF whole-archive flags.

## ya-ncdu

The application no longer imports a prebuilt `libpreview.so`. It builds the
`third_party/preview_lib` Git submodule through CMake, links
`preview::preview`, and therefore receives the correct platform filename and
runtime dependency automatically.

`scripts/configure.sh` uses the PDFium-provided Clang and Ninja so the complete
archive is linked by a compatible toolchain. On Linux it selects the host GCC
libstdc++ installation; on macOS it selects the Xcode SDK, arm64 architecture,
and macOS 13 deployment target.

## Native macOS acceptance commands

```sh
cd ya-ncdu
git submodule update --init --recursive
cd third_party/preview_lib
./scripts/bootstrap_dependencies.sh
./scripts/build.sh dev
./scripts/build.sh asan

cd ../..
./scripts/bootstrap_dependencies.sh
make run
ctest --test-dir build --output-on-failure
./build/ya-ncdu --path third_party/preview_lib/tests/corpus
```

The final native audit should additionally run `file`, `otool -L`, and
`nm -gU` on both `libpreview.dylib` and `ya-ncdu`, verify arm64 slices only,
confirm that only intended preview API symbols are exported, and manually view
text, PNG/JPEG, and PDF previews.
