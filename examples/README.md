# Terminal demo

`preview_demo` is a manual example, not an installed production target. It uses
only `preview::preview` and the public `preview/preview.hpp` header.

Build it after the normal dependency bootstrap:

```sh
cd ~/preview_lib
./scripts/bootstrap_dependencies.sh
cmake --preset dev -DPREVIEW_BUILD_EXAMPLES=ON
cmake --build --preset dev --target preview_demo
```

On macOS arm64, replace both occurrences of `dev` with `mac-dev`. Its binary is
then located at `build/mac-dev/examples/preview_demo`.

Try the included fixtures:

```sh
# Text
./build/dev/examples/preview_demo README.md

# Explicit hex mode
./build/dev/examples/preview_demo tests/corpus/sample.png --mode hex

# Image rendered with ANSI truecolor half-blocks
./build/dev/examples/preview_demo tests/corpus/sample.png --width 40 --height 15
./build/dev/examples/preview_demo tests/corpus/sample.jpg --width 60 --height 20

# PDF page rendered in the terminal
./build/dev/examples/preview_demo tests/corpus/hello_world.pdf --page 0

# Metadata only
./build/dev/examples/preview_demo tests/corpus/sample.gif --mode metadata

# Encrypted PDF fixture. Its owner password is UTF-8 "âge".
./build/dev/examples/preview_demo tests/corpus/encrypted.pdf --password âge
```

The renderer expects a terminal with 24-bit ANSI color support. It uses one
terminal cell for two vertical pixels via `▀`. Kitty/Sixel detection is outside
the library and intentionally not used by this minimal example.
