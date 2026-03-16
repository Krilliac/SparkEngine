# SDK / Public API Surface Audit

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

SparkSDK has 6 well-maintained headers (365 LOC total) exposing 18 subsystem getters via IEngineContext. However, ECS is not exposed (critical gap), a `std::unique_ptr` is exported across DLL boundary (ABI risk), and IGameModule is an internal header used as if it were public.

---

## SDK Contents

All in `/SparkSDK/Include/Spark/`:

| File | Lines | Purpose |
|------|-------|---------|
| SparkSDK.h | 19 | Master include |
| SparkExport.h | 42 | DLL export/import macros (SPARK_MODULE_API, SPARK_GAME_API) |
| Version.h | 45 | Engine/SDK version constants and compatibility checks |
| IModule.h | 87 | Core module interface (5 virtual methods, 2 optional) |
| IEngineContext.h | 143 | Service locator with 18 subsystem getters |
| ModuleRegistry.h | 29 | SPARK_IMPLEMENT_MODULE() boilerplate macro |

**Total: 365 LOC** — compact and well-scoped.

---

## IModule Interface

```cpp
class IModule {
    virtual ~IModule() = default;
    virtual ModuleInfo GetModuleInfo() const = 0;
    virtual bool OnLoad(IEngineContext* context) = 0;
    virtual void OnUnload() = 0;
    virtual void OnUpdate(float deltaTime) = 0;
    virtual void OnRender() {}         // optional
    virtual void OnResize(int w, int h) {}  // optional
};
```

Well-defined, stable, minimal. Virtual destructors present.

---

## Subsystem Exposure

### Exposed via IEngineContext (18 getters)

Graphics, Input, Timer, Audio, Physics, Animation, AI, Events, Networking (optional), SceneManager (optional), Scripting (optional), SaveSystem (optional), CoroutineScheduler (optional) + IsHeadless(), GetEngineVersion(), GetSDKVersion(), InitializeAll(), ShutdownAll()

### NOT Exposed (critical gaps)

| System | Impact |
|--------|--------|
| **ECS (EntityRegistry)** | CRITICAL — game modules can't create/query entities |
| **UI system** | HIGH — no HUD/menu access from modules |
| **2D rendering** | MEDIUM |
| **Dialogue** | MEDIUM |
| **Modding** | MEDIUM |
| **VR** | LOW |
| **Procedural, Destruction, Replay, Stats** | LOW — orphaned anyway |

---

## Critical Issues

### 1. std::unique_ptr Export Across DLL Boundary

**File:** `SparkGame/Source/Core/Main.cpp:28`
```cpp
SPARK_GAME_API std::unique_ptr<Game> g_game;
```

Exporting `std::unique_ptr<T>` across DLL boundary is **undefined behavior** if engine and game DLL use different CRT versions (e.g., `/MD` vs `/MT`, or v142 vs v143 toolset). Crashes on destruction.

**Fix:** Export raw pointer with explicit lifecycle, or ensure matched CRT at build time.

### 2. IGameModule Not in SDK

**File:** `SparkEngine/Source/Core/IGameModule.h` (internal header)

SparkGame.h includes `Core/IGameModule.h` which is NOT in the SDK directory. Creates hidden dependency. SparkGameModule implements both IModule (SDK) and IGameModule (internal) simultaneously — a legacy compatibility hack.

**Fix:** Move IGameModule.h to SDK, or remove legacy interface entirely.

### 3. String Ownership Undocumented

```cpp
virtual const char* GetGameName() const = 0;   // Who owns the string?
virtual const char* GetGameVersion() const = 0;
```

No ownership semantics documented. Implicit: module owns, lifetime must outlast call.

---

## What's Done Right

- All virtual interfaces have virtual destructors
- No exported templates across DLL boundary
- Forward declarations used for all engine types (no internal includes)
- Excellent Doxygen comments on all public headers
- Two-stage versioning (engine version + SDK ABI version)
- Both modern (IModule) and legacy (IGameModule) supported
- SPARK_IMPLEMENT_MODULE() macro eliminates boilerplate

---

## DLL Entry Points

### Modern API (correct)
```cpp
extern "C" {
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
```

### Legacy API (correct)
```cpp
extern "C" {
    SPARK_GAME_API IGameModule* CreateGameModule();
    SPARK_GAME_API void DestroyGameModule(IGameModule* module);
}
```

---

## Action Required

| Issue | Priority | Fix |
|-------|----------|-----|
| `unique_ptr` DLL export | CRITICAL | Replace with raw pointer + lifecycle functions |
| ECS not in SDK | HIGH | Add GetEntityRegistry() to IEngineContext |
| IGameModule not in SDK | HIGH | Move to SDK or deprecate |
| Missing SDK usage guide | MEDIUM | Create SparkSDK/README.md |
| String ownership docs | LOW | Document or use string_view |
