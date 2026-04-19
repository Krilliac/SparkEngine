# SparkBuild consolidation into SparkEngine repo

**Type:** Decision
**Date:** 2026-04-19
**Status:** Active

## What changed

SparkBuild source was vendored into this repo at `SparkBuild/` (pinned to
`Krilliac/SparkBuild@83060506`). The prior pattern — committing prebuilt
`tools/SparkBuild[.exe|-linux-gcc|-linux-clang|-macos]` binaries refreshed by
a weekly `update-sparkbuild.yml` workflow — was retired.

Deleted in the same change:

- `tools/SparkBuild.exe`, `SparkBuild-linux-gcc`, `SparkBuild-linux-clang`,
  `SparkBuild-macos`, `SparkBuild-linux` (symlink)
- `tools/update-sparkbuild.sh`, `tools/update-sparkbuild.ps1`
- `.github/workflows/update-sparkbuild.yml`
- Matching entries in `.gitignore` and `.gitattributes`

## Why

- Committed binaries bloat the repo and are opaque to code review.
- Version-drift across two repos added a cross-repo round-trip to every
  SparkBuild patch.
- SparkBuild only shells out to `cmake` — it has zero compile- or link-time
  dependency on any SparkEngine header or library, so in-tree hosting is
  **not** a circular dependency. Only external deps are WinHTTP/shell32/COM
  on Windows and `pthread` on Linux/macOS.

Bootstrap note: to *use* SparkBuild, a user first invokes plain `cmake` to
build it — same two-stage flow as SparkLauncher / SparkConsole /
SparkShaderCompiler.

## Build wiring

- `CMakeLists.txt:136` — `option(ENABLE_SPARKBUILD ... ON)`
- `CMakeLists.txt:~1390` — `add_subdirectory(SparkBuild)` guarded by option
- `CMakeLists.txt:~2158` — status line `SparkBuild: ${ENABLE_SPARKBUILD}`
- `SparkBuild/CMakeLists.txt` — C++17, builds `${CMAKE_BINARY_DIR}/bin/SparkBuild`, `install(TARGETS SparkBuild RUNTIME DESTINATION bin)` for CPack
- `.github/workflows/build.yml` clang-format find list now includes
  `SparkBuild/src`
- Engine's per-platform `cp -r build/bin/*` packaging step already captures
  the new binary — no dedicated artifact upload needed.

## Relationship to SparkLauncher

**Independent siblings.** Launcher = GUI ImGui project-picker for end users.
SparkBuild = TUI developer build configurator. Different audiences, different
UI toolkits, different invocation contexts. A future optional enhancement
could add a "Configure Build" button in SparkLauncher that shells out to
`./SparkBuild` — this would be soft runtime coupling only, no link-time
coupling.

## Sync policy

If upstream `Krilliac/SparkBuild` gets a hotfix we want in-tree,
diff-and-port (see `SparkBuild/UPSTREAM.md`) — do **not** blind-overwrite
the vendored tree, because we apply our clang-format style on commit.
