# Git submodule publication

`ya-ncdu` is prepared to consume this repository at
`third_party/preview_lib`. CMake uses `add_subdirectory`, and the bootstrap
script initializes the configured submodule before preparing PDFium.

The gitlink cannot be created until `preview_lib` has its own Git repository,
at least one pushed commit, and a reachable remote URL. After publishing it,
run from the `ya-ncdu` repository:

```sh
git submodule add https://github.com/kiquxd/preview_lib.git third_party/preview_lib
git add .gitmodules third_party/preview_lib
```

Pin releases by checking out a tag or commit inside the submodule and then
committing the updated gitlink in `ya-ncdu`:

```sh
git -C third_party/preview_lib checkout v0.1.0
git add third_party/preview_lib
```

For local development before publication, configure with
`PREVIEW_ROOT=/path/to/preview_lib`; this override does not alter the pinned
submodule revision.
