# Third-Party Dependencies Audit

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

7 dependencies total: 6 git submodules (ALL uninitialized) + 1 vendored library. All licenses permissive (MIT/ZLib). curl is dead code (declared but never built/linked). Build silently disables features when submodules are missing. No version tracking file exists.

---

## Dependency Inventory

| Library | Purpose | License | Type | In Repo? | Built? | Linked? |
|---------|---------|---------|------|----------|--------|---------|
| EnTT | ECS | MIT | Header-only (submodule) | NO (uninitialized) | Stub fallback | YES (include) |
| Bullet3 | Physics | ZLib | Compiled (submodule) | NO (uninitialized) | If exists | YES (conditional) |
| Dear ImGui | Editor UI | MIT | Compiled (submodule) | NO (uninitialized) | If exists + ENABLE_EDITOR | YES (editor only) |
| AngelScript | Scripting | ZLib | Compiled (submodule) | NO (uninitialized) | If exists | YES (conditional) |
| curl | HTTP (crash reports) | MIT | Compiled (submodule) | NO (uninitialized) | **NEVER** | **NEVER** |
| miniz | Compression | MIT | Compiled (submodule) | NO (uninitialized) | If exists | YES |
| tinyobjloader | OBJ mesh loading | MIT | Header-only (vendored) | **YES** (3,517 lines) | YES | YES |

---

## Critical Issues

### 1. All 6 Submodules Uninitialized

Every git submodule directory is empty. Build system uses `if(EXISTS ...)` to detect and silently disable features:
```
[MISSING] EnTT → uses 332-line custom stub
[MISSING] Bullet3 → PhysicsSystem disabled
[MISSING] ImGui → Editor disabled
[MISSING] AngelScript → Scripting disabled
[MISSING] miniz → Compression disabled
```

**Impact**: Builds succeed but most engine features don't work. Creates false confidence.

**Fix**: `git submodule update --init --recursive` or fail with `message(FATAL_ERROR ...)` for critical deps.

### 2. curl is Dead Code

- Declared in `.gitmodules`
- Checked in CMakeLists.txt:1187
- **Never added to any build target** via add_subdirectory() or target_link_libraries()
- Referenced in CrashHandler.cpp (`#include <curl/curl.h>`) but link will fail
- Has extensive CVE history — shouldn't be in repo if unused

**Fix**: Remove from `.gitmodules` and ThirdParty/, or fully integrate.

### 3. No Version Tracking

No `VERSIONS.txt`, `DEPENDENCIES.md`, or CMake version specifications. Versions only identifiable by inspecting git commit hashes in `.gitmodules`.

| Library | Commit Hash | Version (if known) |
|---------|-------------|-------------------|
| EnTT | 9fdc43f... | Unknown (commit hash only) |
| Bullet3 | 63c4d67... | Unknown |
| Dear ImGui | 934c6a5... | docking branch |
| AngelScript | c81df25... | Unknown (mirror repo) |
| curl | 4a15bc1... | Unknown |
| miniz | 4b9fcf1... | Unknown |
| tinyobjloader | N/A | v2.0.0 (in header) |

### 4. EnTT Stub Fallback

When the EnTT submodule is missing (always, since uninitialized), a 332-line custom `entt_stub/` provides a minimal ECS interface. This is fragile — API drift between stub and real EnTT will cause compile failures when submodule is eventually initialized.

---

## License Compliance

**GPL Contamination Risk: NONE**

All dependencies use permissive licenses (MIT, ZLib). Safe for commercial/proprietary use. No license conflicts detected.

---

## Security Assessment

| Library | Risk | Notes |
|---------|------|-------|
| curl | MODERATE | Extensive CVE history. Dead code but if ever linked, needs periodic updates |
| Bullet3 | LOW | Stable since ~2020, few CVEs |
| miniz | LOW | Minimal attack surface |
| tinyobjloader | LOW | Basic parser, no network I/O |
| Dear ImGui | LOW | Editor-only, not shipped in release |
| EnTT | LOW | Pure C++ ECS, no I/O |
| AngelScript | LOW | Sandboxed scripting |

---

## CMake Integration

**ThirdParty/CMakeLists.txt** (80 lines):
- Defines `entt` (INTERFACE), `imgui` (STATIC), `angelscript` (subdirectory)
- Platform-specific ImGui backends (Win32+DX11 or SDL2+OpenGL3)

**Main CMakeLists.txt**:
- miniz: GLOB sources, build as STATIC
- tinyobjloader: find_path() + STATIC lib from wrapper
- bullet3: add_subdirectory() if exists
- curl: Checked but never built (orphaned)

**Duplicate imgui target**: Both ThirdParty/CMakeLists.txt and SparkEditor/CMakeLists.txt define imgui build targets (previously identified in cmake-build-audit.md).

---

## Recommendations

| Issue | Priority | Action |
|-------|----------|--------|
| Initialize submodules | CRITICAL | `git submodule update --init --recursive` |
| Remove curl | HIGH | Delete from .gitmodules + ThirdParty/ |
| Add VERSIONS.txt | HIGH | Document all pinned commits and dates |
| Fail on missing critical deps | HIGH | Use FATAL_ERROR for Bullet3, miniz |
| Replace entt_stub | MEDIUM | Initialize real EnTT submodule |
| Fix duplicate imgui target | MEDIUM | Single definition in ThirdParty/ |
| Add CVE audit script | LOW | Periodic check for vendored library CVEs |
