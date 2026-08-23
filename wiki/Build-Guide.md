# SparkEngine Build Guide

> **Audience:** Programmers and contributors
>
> **Thread Context:** Not applicable — build workflow
>
> **Platform/Backend Scope:** Windows primary; Linux and macOS experimental

## Supported build path

SparkEngine requires CMake 3.25+ and a C++23-capable compiler. Windows 10+ with MSVC is the primary platform; Linux and macOS support is experimental.

```bash
# Configure one supported preset
cmake --preset windows-release
cmake --preset linux-gcc-release
cmake --preset macos-release

# Build
cmake --build build --config Release

# Test
cd build && ctest --output-on-failure
```

## Read before building

- [Getting Started](getting-started/Getting-Started.md) — prerequisites and the normal build sequence.
- [System Requirements](platform/System-Requirements.md) — supported hardware and platforms.
- [Build System and CMake Modules](advanced/Build-System-and-CMake-Modules.md) — presets, options, and CMake architecture.
- [Cross-Compilation and Wine Testing](platform/Cross-Compilation-Wine-Testing.md) — Windows-target builds from Linux.
- [CI Reproducible Builds](development/CI-Reproducible-Builds.md) — commands matching continuous integration.
- [Troubleshooting](advanced/Troubleshooting.md) — common configure, build, runtime, and graphics failures.

The authoritative preset definitions are in [`CMakePresets.json`](https://github.com/Krilliac/SparkEngine/blob/Working/CMakePresets.json).

## Source & Freshness

This guide summarizes the canonical presets on `Working`; `CMakePresets.json` and the linked platform pages are authoritative for exact options.
