# SDK / Public API Surface Audit

**Last updated:** 2026-03-21
**Type:** Observation
**Status:** Resolved
**Severity:** Low (all critical and high issues fixed)

## Description

SparkSDK has 10 well-maintained headers exposing 26 subsystem getters via IEngineContext, plus math types, input types, event documentation, and a logging interface. SDK ABI version bumped to v2.

---

## SDK Contents (v2)

All in `/SparkSDK/Include/Spark/`:

| File | Purpose |
|------|---------|
| SparkSDK.h | Master include (includes all SDK headers) |
| SparkExport.h | DLL export/import macros (SPARK_MODULE_API, SPARK_GAME_API) |
| Version.h | Engine/SDK version constants and compatibility checks (SDK v2) |
| IModule.h | Module interface: 4 required + 5 optional lifecycle methods |
| IEngineContext.h | Service locator with 26 subsystem getters + engine state queries |
| ModuleRegistry.h | SPARK_IMPLEMENT_MODULE() and SPARK_MODULE_DEPENDENCIES() macros |
| ILogger.h | Abstract logging interface for modules |
| MathTypes.h | Vec2/Vec3/Vec4/Quat/Color/Mat4x4/AABB/Ray types |
| InputTypes.h | MouseButton/GamepadButton/GamepadAxis/InputAction types |
| EventTypes.h | Event system usage guide and built-in event type reference |

---

## SDK v2 Changes (2026-03-21)

### New subsystem getters in IEngineContext (7 added)

- `GetReplay()` — ReplaySystem (recording/playback)
- `GetLocalization()` — LocalizationSystem (i18n)
- `GetTween()` — TweenSystem (interpolation/animation)
- `GetAbilities()` — AbilitySystem (spells/auras/procs)
- `GetDestruction()` — DestructionSystem (destructible objects)
- `GetCinematic()` — SequencerManager (cutscenes)
- `GetVR()` — VRSystem (VR/AR integration)

### New engine state queries

- `GetElapsedTime()` — total runtime in seconds
- `GetFrameNumber()` — monotonically increasing frame counter

### New IModule lifecycle hooks (4 added)

- `OnFixedUpdate(float)` — deterministic fixed-timestep update
- `OnPause()` — called when game is paused
- `OnResume()` — called when game resumes
- `OnImGui()` — debug UI rendering during ImGui pass

### New SDK headers (4 added)

- `ILogger.h` — abstract logging interface for modules
- `MathTypes.h` — Vec2, Vec3, Vec4, Quat, Color, Mat4x4, AABB, Ray
- `InputTypes.h` — MouseButton, GamepadButton, GamepadAxis, InputAction
- `EventTypes.h` — event system usage guide and built-in event reference

### String ownership documented

IModule.h now documents that const char* returns from GetModuleInfo() must point to module-owned memory (string literals recommended).

### SDK ABI version bumped to 2

---

## Resolved Issues

| Issue | Resolution |
|-------|-----------|
| `unique_ptr` DLL export | **FIXED** (v1) — Raw pointer with manual lifecycle |
| ECS not in SDK | **FIXED** (v1) — GetWorld() added |
| Missing subsystem getters | **FIXED** (v2) — 7 new getters for Replay/Localization/Tween/Ability/Destruction/Cinematic/VR |
| String ownership undocumented | **FIXED** (v2) — Documented in IModule.h |
| No math types in SDK | **FIXED** (v2) — MathTypes.h with DirectXMath-compatible types |
| No input types in SDK | **FIXED** (v2) — InputTypes.h with mouse/gamepad/action types |
| No event documentation | **FIXED** (v2) — EventTypes.h with usage guide |
| No logging interface | **FIXED** (v2) — ILogger.h abstract interface |
| No fixed-timestep hook | **FIXED** (v2) — OnFixedUpdate() in IModule |
| No pause/resume hooks | **FIXED** (v2) — OnPause()/OnResume() in IModule |

## Remaining Issues

| Issue | Priority | Fix |
|-------|----------|-----|
| IGameModule not in SDK | LOW | Legacy path; modern IModule is preferred |
