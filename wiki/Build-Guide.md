# SparkEngine Build Guide

> **Audience:** Programmers and contributors
>
> **Thread Context:** Not applicable — build workflow
>
> **Platform/Backend Scope:** The only declared release profile is blocked and
> uncertified `stable-v1` on Windows 11 x64/MSVC v143 with D3D11 and Windows
> NullRHI, plus the required Windows products enumerated in readiness. Windows 10,
> other toolchains/backends, Linux, and macOS are development
> or experimental paths outside it.

## Documented build paths

SparkEngine requires CMake 3.25+ and a C++23-capable compiler. The in-profile
candidate path is Windows 11 x64 with MSVC v143; Windows 10, Linux, and macOS
instructions document development builds rather than release support.

Choose the preset for the host platform and use its matching build directory:

```powershell
# Windows
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/windows-release -C Release --output-on-failure --no-tests=error
```

```bash
# Linux
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --test-dir build/linux-gcc-release --output-on-failure --no-tests=error

# macOS
cmake --preset macos-release
cmake --build --preset macos-release
ctest --test-dir build/macos-release --output-on-failure --no-tests=error
```

## Read before building

- [Getting Started](getting-started/Getting-Started.md) — prerequisites and the normal build sequence.
- [System Requirements](platform/System-Requirements.md) — build requirements, development paths, and the narrow release boundary.
- [Build System and CMake Modules](advanced/Build-System-and-CMake-Modules.md) — presets, options, and CMake architecture.
- [Cross-Compilation and Wine Testing](platform/Cross-Compilation-Wine-Testing.md) — Windows-target builds from Linux.
- [CI Reproducible Builds](development/CI-Reproducible-Builds.md) — commands matching continuous integration.
- [Troubleshooting](advanced/Troubleshooting.md) — common configure, build, runtime, and graphics failures.

The authoritative preset definitions are in [`CMakePresets.json`](https://github.com/Krilliac/SparkEngine/blob/Working/CMakePresets.json).

## Source & Freshness

This guide summarizes the canonical presets on `Working`; `CMakePresets.json` and the linked platform pages are authoritative for exact options.
