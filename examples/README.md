# Terminal demo

`preview_demo` uses only the public header and `preview::preview`. Build it with:

```sh
./scripts/bootstrap_dependencies.sh
cmake --preset dev -DPREVIEW_BUILD_EXAMPLES=ON
cmake --build --preset dev --target preview_demo
```

On macOS arm64, replace `dev` with `mac-dev`.

```sh
./build/dev/examples/preview_demo README.md
./build/dev/examples/preview_demo tests/corpus/sample.png --mode hex
./build/dev/examples/preview_demo tests/corpus/sample.png --width 40 --height 15
./build/dev/examples/preview_demo tests/corpus/sample.jpg --width 60 --height 20
./build/dev/examples/preview_demo tests/corpus/sample.gif --mode metadata
./build/dev/examples/preview_demo tests/corpus/hello_world.pdf
```

Text/hex is printed directly and images use ANSI 24-bit color with `▀`, one
terminal cell per two vertical pixels. A PDF demonstrates structured detection
and the external-viewer message; the library does not launch a program.
