# CMake Build System Audit

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** Medium

## Description

Audit of CMakeLists.txt files for dead options, stale references, and build system bloat.

## Dead Build Options (Define Flags Never Checked in Code)

### Critical: Options With No Backend

| Option | Flag Defined | Status |
|--------|-------------|--------|
| `ENABLE_LUA` | `LUA_SCRIPTING_ENABLED` | ThirdParty/Scripting/lua/ does NOT exist. No code checks flag. |
| `ENABLE_PHYSX` | `PHYSX_AVAILABLE` | ThirdParty/Physics/PhysX/ does NOT exist. No code checks flag. |

**Action:** Delete both options and their header search code entirely.

### Medium: Options That Set Flags But No Code Guards Exist

| Option | Flag Defined | Lines |
|--------|-------------|-------|
| `ENABLE_COLLABORATIVE` | `COLLABORATIVE_ENABLED` | 791-793 |
| `ENABLE_GRAPHICS` | `GRAPHICS_ENABLED` | 787-789 |
| `ENABLE_ASSET_STREAMING` | `ASSET_STREAMING_ENABLED` | 803-805 |
| `ENABLE_HOT_RELOAD` | `HOT_RELOAD_ENABLED` | 807-809 |
| `ENABLE_TERRAIN_SYSTEM` | `TERRAIN_SYSTEM_ENABLED` | 811-813 |
| `ENABLE_ADVANCED_INPUT` | `ADVANCED_INPUT_ENABLED` | 823-825 |

6 options define compile flags that are never `#ifdef`'d in any source file.

**Action:** Remove options or add actual code guards.

## Build Options That Work Correctly

- `PROFILING_ENABLED` — Used in Profiler.h
- `BUILD_TESTS` — Controls test compilation
- `ENABLE_EDITOR` — Controls editor build
- `ENABLE_DXR` — Controls DXR support
- `ENABLE_NETWORKING` — Controls networking code
- `ENABLE_VULKAN`, `ENABLE_OPENGL` — Control RHI backends

## Duplicate imgui Target Definitions

- `ThirdParty/CMakeLists.txt`: 3 `add_library(imgui)` calls (Win32+DX11, SDL2+OpenGL3, core-only)
- `SparkEditor/CMakeLists.txt`: 2 more `add_library(imgui)` calls (guarded by `if(NOT TARGET imgui)`)

Guard prevents actual duplication at build time, but the code is scattered and fragile.

**Action:** Remove imgui definitions from SparkEditor/CMakeLists.txt entirely.

## Stale Third-Party References

- `ThirdParty/Networking/curl/` — Directory exists but is empty. CMake checks for `curl/CMakeLists.txt` which doesn't exist. Reports "MISSING" in dependency status.

## Circular Linking Pattern

SparkEngine executable links both SparkGame (SHARED) and SparkEngineLib (STATIC). SparkGame also links SparkEngineLib. This creates a circular reference because SparkConsole code in SparkEngineLib references Game/Player symbols from SparkGame.

Documented with comments but architecturally fragile.

## Summary

| Issue | Count | Severity |
|-------|-------|----------|
| Dead build options (no backend) | 2 | Critical |
| Dead build flags (no code guards) | 6 | Medium |
| Duplicate target definitions | 1 (imgui) | Medium |
| Empty third-party directories | 1 (curl) | Low |
| Circular linking | 1 pattern | Low |
