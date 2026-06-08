# SparkBuild In-Tree

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A — build-tooling decision.
>
> **Platform/Backend Scope:** Cross-platform developer tool. Windows external deps: WinHTTP/shell32/COM. Linux/macOS external dep: `pthread`. No dependency on any SparkEngine header or library.

## Overview

SparkBuild is a TUI developer build configurator. Its source was vendored directly into this repository at `SparkBuild/` (pinned to `Krilliac/SparkBuild@83060506`), replacing the prior pattern of committing prebuilt per-platform binaries under `tools/` that were refreshed by a weekly workflow. SparkBuild only shells out to `cmake`; it has zero compile- or link-time dependency on the engine, so hosting it in-tree is not a circular dependency.

## What Changed

In-tree hosting replaced committed binaries. Deleted in the same change:

- `tools/SparkBuild.exe`, `SparkBuild-linux-gcc`, `SparkBuild-linux-clang`, `SparkBuild-macos`, and the `SparkBuild-linux` symlink
- `tools/update-sparkbuild.sh`, `tools/update-sparkbuild.ps1`
- `.github/workflows/update-sparkbuild.yml`
- Matching `.gitignore` / `.gitattributes` entries

The vendored tree now lives at `SparkBuild/` with `CMakeLists.txt`, `src/`, `resources/`, `README.md`, and `UPSTREAM.md`.

## Why

- Committed binaries bloat the repo and are opaque to code review.
- Maintaining two repos added a cross-repo round-trip to every SparkBuild patch.
- SparkBuild only invokes `cmake`, so there is no circular dependency. External deps are only WinHTTP/shell32/COM on Windows and `pthread` on Linux/macOS.

**Bootstrap note:** to *use* SparkBuild you first invoke plain `cmake` to build it — the same two-stage flow as SparkLauncher, SparkConsole, and SparkShaderCompiler.

## Build Wiring

- `CMakeLists.txt` — `option(ENABLE_SPARKBUILD ... ON)` (around line 136)
- `CMakeLists.txt` — `add_subdirectory(SparkBuild)` guarded by the option (around line 1390)
- `CMakeLists.txt` — status line `SparkBuild: ${ENABLE_SPARKBUILD}` (around line 2158)
- `SparkBuild/CMakeLists.txt` — C++17, builds `${CMAKE_BINARY_DIR}/bin/SparkBuild`, `install(TARGETS SparkBuild RUNTIME DESTINATION bin)` for CPack
- `.github/workflows/build.yml` — clang-format find list includes `SparkBuild/src`
- The engine's per-platform `cp -r build/bin/*` packaging step already captures the binary — no dedicated artifact upload needed.

## Relationship to SparkLauncher

**Independent siblings.** SparkLauncher is a GUI ImGui project-picker for end users; SparkBuild is a TUI developer build configurator. Different audiences, UI toolkits, and invocation contexts. A future optional enhancement could add a "Configure Build" button in SparkLauncher that shells out to `./SparkBuild` — runtime coupling only, never link-time.

## Sync Policy

If upstream `Krilliac/SparkBuild` gets a hotfix worth pulling in-tree, **diff-and-port** (see `SparkBuild/UPSTREAM.md`). Do **not** blind-overwrite the vendored tree, because the project's clang-format style is applied on commit.

## Source & Freshness

- **Original entry date:** 2026-04-19 (`sparkbuild-in-tree-2026-04-19.md`, type: Decision)
- **Verified against codebase 2026-06-08.**
- Status bullets:
  - **Still holds.** `SparkBuild/` exists with `CMakeLists.txt`, `src/`, `resources/`, `README.md`, `UPSTREAM.md`.
  - **Old binaries gone** — no `tools/SparkBuild*` artifacts remain.
  - `add_subdirectory(SparkBuild)` confirmed present in the root `CMakeLists.txt` (line numbers approximate; the file has shifted since the original note).

## Related Pages

- [Daemon Services Architecture](Daemon-Services-Architecture.md) — sibling developer tooling
- [Wine Role and Fallback Tiers](Wine-Role-and-Fallback-Tiers.md)
