# SDK / Public API Surface Audit

**Last updated:** 2026-03-17
**Type:** Observation
**Status:** Mostly Resolved
**Severity:** Medium (was High — critical issues fixed)

## Description

SparkSDK has 6 well-maintained headers (~370 LOC total) exposing 19 subsystem getters via IEngineContext. Most critical issues have been resolved.

---

## SDK Contents

All in `/SparkSDK/Include/Spark/`:

| File | Lines | Purpose |
|------|-------|---------|
| SparkSDK.h | 19 | Master include |
| SparkExport.h | 42 | DLL export/import macros (SPARK_MODULE_API, SPARK_GAME_API) |
| Version.h | 45 | Engine/SDK version constants and compatibility checks |
| IModule.h | 87 | Core module interface (5 virtual methods, 2 optional) |
| IEngineContext.h | ~150 | Service locator with 19 subsystem getters (including ECS World) |
| ModuleRegistry.h | 29 | SPARK_IMPLEMENT_MODULE() boilerplate macro |

---

## Resolved Issues (2026-03-17)

### 1. unique_ptr DLL Export — FIXED

`g_game` in SparkGame changed from `std::unique_ptr<Game>` to `Game*` with explicit lifecycle management in SparkGameModule::Initialize/Shutdown. Eliminates ABI/CRT mismatch crash risk.

### 2. ECS Not Exposed — FIXED

Added `GetWorld()` / `const GetWorld()` to IEngineContext. Game modules can now access the ECS World (and through it, the EnTT registry) via `context->GetWorld()`.

---

## Remaining Issues

### IGameModule Not in SDK

**File:** `SparkEngine/Source/Core/IGameModule.h` (internal header)

SparkGameModule implements both IModule (SDK) and IGameModule (internal). This is a legacy compatibility path. Not critical — the modern IModule API is the preferred path.

### String Ownership Undocumented

`GetGameName()` and `GetGameVersion()` return `const char*` with no documented ownership semantics. Implicit: module owns, lifetime must outlast call.

---

## What's Done Right

- All virtual interfaces have virtual destructors
- No exported templates across DLL boundary
- Forward declarations used for all engine types
- Excellent Doxygen comments on all public headers
- Two-stage versioning (engine version + SDK ABI version)
- Both modern (IModule) and legacy (IGameModule) supported

---

## Action Required

| Issue | Priority | Fix |
|-------|----------|-----|
| `unique_ptr` DLL export | ~~CRITICAL~~ | **RESOLVED** |
| ECS not in SDK | ~~HIGH~~ | **RESOLVED** (GetWorld() added) |
| IGameModule not in SDK | MEDIUM | Move to SDK or deprecate |
| Missing SDK usage guide | LOW | Create SparkSDK/README.md |
| String ownership docs | LOW | Document or use string_view |
