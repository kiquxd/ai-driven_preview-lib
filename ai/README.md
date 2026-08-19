# AI handoff directory

This directory is the single location for planning and handoff material created
by coding agents for `preview_lib`.

Contents:

- `PLAN.md` — implementation scope, architecture, milestones and acceptance
  criteria.
- `DEPENDENCIES.md` — dependency policy, build/runtime closure and licensing
  requirements.
- `IMPLEMENTATION.md` — delivered MVP, verification evidence, encountered
  problems and explicit remaining limitations.
- `MACOS_SUPPORT.md` — supported-platform build details and native acceptance
  checklist.
- `YA_NCDU_INTEGRATION.md` — TUI integration boundary and verification notes.
- `SUBMODULE_SETUP.md` — publication and Git submodule pinning procedure.

Future agent-authored plans, reviews, investigations, benchmark notes and
handoff reports must be placed under `ai/`. Production sources, public headers,
tests, fixtures, CMake files, vendored third-party sources and generated license
files do not belong here because they are part of the product/build itself.

Do not create additional planning documents at the repository root.
