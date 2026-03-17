# CMake Build System Audit

**Last updated:** 2026-03-17
**Type:** Observation
**Status:** Mostly Resolved
**Severity:** Low (was Medium)

## Description

Audit of CMakeLists.txt files for dead options, stale references, and build system bloat.

## Resolved Issues

### Dead Build Options — RESOLVED (prior sessions)

`ENABLE_LUA` and `ENABLE_PHYSX` were removed in a prior session. No dead-backend options remain.

### Dead curl Dependency — RESOLVED (2026-03-17)

Removed curl submodule entry from `.gitmodules`, deleted empty `ThirdParty/Networking/curl/` directory, and removed curl dependency check from `CMakeLists.txt`.

### Dead Build Flags — RESOLVED (prior sessions)

The 6 options (`ENABLE_COLLABORATIVE`, `ENABLE_GRAPHICS`, `ENABLE_ASSET_STREAMING`, `ENABLE_HOT_RELOAD`, `ENABLE_TERRAIN_SYSTEM`, `ENABLE_ADVANCED_INPUT`) that set flags with no code guards were removed in a prior session.

## Remaining Issues

### Duplicate imgui Target Definitions

- `ThirdParty/CMakeLists.txt`: 3 `add_library(imgui)` calls (Win32+DX11, SDL2+OpenGL3, core-only)
- `SparkEditor/CMakeLists.txt`: 2 more `add_library(imgui)` calls (guarded by `if(NOT TARGET imgui)`)

Guard prevents actual duplication at build time, but the code is scattered.

### Circular Linking Pattern

SparkEngine links both SparkGame (SHARED) and SparkEngineLib (STATIC). SparkGame also links SparkEngineLib. Documented with comments but architecturally fragile.

## Build Options That Work Correctly

- `PROFILING_ENABLED` — Used in Profiler.h
- `BUILD_TESTS` — Controls test compilation
- `ENABLE_EDITOR` — Controls editor build
- `ENABLE_DXR` — Controls DXR support
- `ENABLE_NETWORKING` — Controls networking code
- `ENABLE_VULKAN`, `ENABLE_OPENGL` — Control RHI backends
- `ENABLE_SDL2` — Auto-enabled on Linux
- `SPARK_HEADLESS_SUPPORT` — Dedicated server mode
- `ENABLE_HYBRID_RT` — Hybrid ray tracing

## Summary

| Issue | Severity | Status |
|-------|----------|--------|
| Dead build options (no backend) | ~~Critical~~ | **RESOLVED** |
| Dead build flags (no code guards) | ~~Medium~~ | **RESOLVED** |
| Dead curl dependency | ~~Low~~ | **RESOLVED** |
| Duplicate imgui target definitions | Low | OPEN (fragile but works) |
| Circular linking | Low | OPEN (documented) |
