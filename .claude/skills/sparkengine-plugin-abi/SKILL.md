---
name: sparkengine-plugin-abi
description: >-
  SparkEngine game-module DLL plugin ABI reference — how ModuleManager discovers
  and loads GameModules/*.dll, the extern "C" injection hooks a module must export
  (SparkModuleInjectEngineContext / Console / ImGui), and the exact checklist for
  adding a new game module DLL. TRIGGER when the user says "add a new game module",
  "my module's EngineContext::Get() returns null", "module console commands don't
  work", "module ImGui draws nothing / crashes", "CreateModule not found", "how does
  ModuleManager load DLLs", "why is my module's NetworkManager a dead singleton", or
  asks how the plugin/module ABI works. DO NOT TRIGGER for finishing the specific
  EngineContext-injection P0 step-by-step (use sparkengine-module-injection-p0-campaign),
  for engine-internal subsystem wiring unrelated to DLLs, or for AngelScript hot-reload
  (that is scripting, not the native module ABI).
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine Plugin / Module DLL ABI

This is the reference for SparkEngine's **native game-module plugin system**: how the
engine finds, loads, and talks to the DLLs under `GameModules/` (and standalone game
projects). Read this when adding a module, or when a loaded module behaves as if it
cannot see the engine (null context, dead singletons, invisible console commands,
blank ImGui).

**Jargon defined once:**
- **Module DLL** — a shared library (`.dll` on Windows, `.so` on Linux, `.dylib` on
  macOS) that implements `Spark::IModule` and exports factory functions. Each folder
  in `GameModules/` builds one. Ten ship in-tree (SparkGame, SparkGameFPS,
  SparkGameMMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript).
- **`ModuleManager`** — the host-side loader (`SparkEngine/Source/Core/ModuleManager.{h,cpp}`).
  Owned by the engine runtime; finds, loads, sorts, initializes, updates, and unloads modules.
- **`IModule`** — the module interface (`SparkSDK/Include/Spark/IModule.h`). Lifecycle:
  `OnLoad → OnUpdate/OnFixedUpdate/OnRender/OnImGui → OnUnload`.
- **`IEngineContext` / `EngineContext`** — the engine service locator. `EngineContext::Get()`
  is the module's window into engine subsystems (graphics, input, physics, network, etc.).
- **`SparkEngineLib`** — the engine as a **static** library. It is linked into the
  executable **and into every module DLL**. This is the root cause of every injection
  hook below: any global inside `SparkEngineLib` (the `g_engineContext` pointer, the
  `SimpleConsole` singleton, ImGui's `GImGui`) exists as a **separate per-image copy**
  inside each module, disconnected from the host's copy until the host injects across
  the DLL boundary.

## When NOT to use this skill

| Situation | Use instead |
|-----------|-------------|
| You need to *finish* the EngineContext-injection P0 step-by-step | `sparkengine-module-injection-p0-campaign` |
| AngelScript hot-reload / script context questions | scripting skills (not the native ABI) |
| Wiring an engine-internal subsystem that is not a DLL | engine wiring / `CLAUDE.md` "Wiring Things In" |

---

## 1. How ModuleManager discovers and loads DLLs

The host resolves modules from three sources, in `ModuleManager` (verified in
`ModuleManager.cpp`):

1. **Manifest** — `LoadModulesFromManifest(path)` reads a `spark.modules.json`-style
   file. Minimal parser: it scans for `"path"` keys and loads each. Relative paths
   resolve against the manifest directory. (No root manifest ships in-tree today —
   directory scan is the live path.)
2. **Directory scan (fallback)** — `LoadModulesFromDirectory(dir)`. This is what the
   shipping engine uses: `SparkEngineWindows.cpp` / `SparkEngineLinux.cpp` call
   `LoadModulesFromManifest` if a manifest exists, else `LoadModulesFromDirectory(exeDir)`.
3. **Explicit** — `LoadModule(path)` for a single DLL (used by manifest/scan and the editor).

### Directory-scan candidate rules (verified `ModuleManager.cpp` ~line 405)

A file in the scanned directory is loaded only if **all** hold:
- Extension matches the platform: `.dll` (Windows) / `.so` (Linux) / `.dylib` (macOS).
- Filename **contains** `Game`, `Module`, or `Plugin`.
- Filename is **not** a system/runtime DLL: rejected if it starts with `d3d`,
  `vcruntime`, `msvcp`, `ucrtbase`, or contains `SparkConsole` / `SparkEngine`.
- **Windows only:** the file is first mapped with `LoadLibraryExA(..., DONT_RESOLVE_DLL_REFERENCES)`
  — which maps the image **without** running `DllMain` or resolving imports — and is
  skipped unless it actually exports `CreateModule` or `CreateGameModule`. This stops
  unrelated third-party DLLs (overlays, middleware) whose names merely contain "Game"
  from being loaded and their `DllMain` executed.

Paths containing `..` are rejected outright (traversal guard, `LoadModule`).

### Load sequence inside `LoadModule` (order matters)

```
LoadLibraryA / dlopen(path)
  └─ (Windows) GetProcAddress + call, BEFORE CreateModule:
       SparkModuleInjectConsole(&SimpleConsole::GetInstance())   // host console
       SparkModuleInjectEngineContext(EngineContext::Get())      // host context
       SparkModuleInjectImGui(ctx, alloc, free, userData)        // host ImGui (if set)
  └─ GetProcAddress("CreateModule") / ("DestroyModule")   // new API, preferred
       └─ instance = CreateModule()
       └─ IsSDKCompatible(info.sdkVersion)  // reject on mismatch, then FreeLibrary
  └─ else GetProcAddress("CreateGameModule") / ("DestroyGameModule")  // legacy API
       └─ wrapped in LegacyModuleAdapter (IGameModule → IModule)
  └─ SortModules()   // numeric loadOrder, or topological if dependencies declared
```

The three injection hooks are called via `GetProcAddress` and are **each optional** —
a module missing a hook simply skips it (no error). They run **before** `CreateModule()`
so the module's constructor and `OnLoad` already see the host's console/context/ImGui.

> The injection hooks are Windows-only in both the host (`#ifdef _WIN32` in `LoadModule`)
> and the SDK (`#if defined(_WIN32)` in `ModuleDllMain.h`). On Linux/macOS the engine
> links modules so that the static `SparkEngineLib` symbols resolve from the executable
> (`ENABLE_EXPORTS`, no `NEEDED` dependency — see `CMakeLists.txt` ~line 1419), so there
> is a single shared copy and no per-image injection is needed.

### Lifecycle calls (all iterate loaded modules in load order)

`InitializeAll(ctx)` → `UpdateAll(dt)` / `FixedUpdateAll(fixedDt)` / `RenderAll()` /
`ImGuiAll()` / `ResizeAll(w,h)` → `ShutdownAll()` (reverse order) → `UnloadAll()`.
`InitializeAll` is driven from `SparkEngineWindows.cpp` / `SparkEngineLinux.cpp` via
`GetEngineRuntime().moduleManager->InitializeAll(EngineContext::Get())`.

---

## 2. The injection ABI — what a module must export

All three hooks are generated for you by including **one** header in **exactly one**
`.cpp` per module:

```cpp
#include <Spark/ModuleDllMain.h>   // defines DllMain + the 3 inject hooks (Windows)
```

`ModuleDllMain.h` (verified) emits these `extern "C" __declspec(dllexport)` functions:

| Export | Signature | What it fixes |
|--------|-----------|---------------|
| `SparkModuleInjectConsole` | `void(void* hostConsole)` | Points the module's per-image `SimpleConsole` at the host's, so module console-command registrations are visible to the engine. Without it, module commands are silently dead on Windows. |
| `SparkModuleInjectEngineContext` | `void(void* ctx)` | Calls `EngineContext::SetInjected((EngineContext*)ctx)`. Points the module's `EngineContext::Get()` at the host's live context. Without it, `Get()` returns the module's null per-image `g_engineContext` and every service lookup (network, physics, world…) fails or falls back to a dead per-module singleton. |
| `SparkModuleInjectImGui` | `void(void* ctx, void* alloc, void* free, void* userData)` | Sets the module's ImGui allocator functions and current context to the exe-owned ones. Without it, module `OnImGui()` draws nothing or crashes (per-image `GImGui`). No-op if the module is built without `SPARK_HAS_IMGUI`. |

### How `EngineContext::Get()` resolves (verified `EngineContext.cpp`)

```cpp
EngineContext* EngineContext::Get() {
    if (g_injectedContext) return g_injectedContext;  // host-injected, preferred
    return g_engineContext.get();                     // per-image owning global
}
void EngineContext::SetInjected(EngineContext* ctx) { g_injectedContext = ctx; }
```

`g_injectedContext` is a **non-owning** raw pointer — module teardown / `FreeLibrary`
must never free the host's context. Never pass the owning `unique_ptr` to `SetInjected`.

### Consuming engine services from inside a module

Prefer the context passed to `OnLoad(IEngineContext* context)` — store it and use its
named getters (`context->GetPhysics()`, `context->GetEventBus()`, …). For free functions
and console-command lambdas that have no context in scope, call
`EngineContext::Get()->GetX()` (works once injection has run). Example from a shipping
module (`SparkGameFPS/Source/Core/Main.cpp`):

```cpp
auto* ctx = EngineContext::Get();
auto* weather = ctx ? ctx->GetWeather() : nullptr;   // always null-check
```

Singletons that were retrofitted to prefer the injected context — e.g.
`NetworkManager::GetInstance()` (verified `NetworkManager.cpp`) — do this internally:

```cpp
NetworkManager& NetworkManager::GetInstance() {
    if (auto* ctx = EngineContext::Get())
        if (auto* svc = ctx->GetNetworkService())
            if (auto* net = dynamic_cast<NetworkManager*>(svc))
                return *net;              // host's real, registered NetworkManager
    static NetworkManager instance;       // fallback (no service registered)
    return instance;
}
```

Once `SparkModuleInjectEngineContext` has run, a module's `NetworkManager::GetInstance()`
returns the **host's** registered manager, not the dead per-image `static`.

> **The single GetInstance rule (consistent across the sparkengine skills):** do **not**
> add *new* naive per-module `GetInstance()` singletons — those are the cross-DLL
> anti-pattern the injection seam removes. What *is* acceptable: (a) the retrofitted
> context-routing `GetInstance()` shown above, which returns the injected host instance;
> and (b) genuinely-shared singletons like `SimpleConsole` and
> `SeamlessAreaManager::GetInstance()`. Prefer `context->GetX()` / `EngineContext::Get()->GetX()`
> at the call site whenever you have it. (See `sparkengine-architecture-contract` Invariant 2
> and `sparkengine-debugging-playbook` row 5 for the same rule.)

---

## 3. Checklist — adding a NEW game module DLL

Drop a folder into `GameModules/<YourModule>/` — CMake auto-discovers any
`GameModules/*/CMakeLists.txt` when `BUILD_GAME_MODULES=ON` (default; verified
`CMakeLists.txt` ~line 1403). Then:

- [ ] **Module class** — subclass `Spark::IModule` (`SparkSDK/Include/Spark/IModule.h`).
      Implement `GetModuleInfo()`, `OnLoad(IEngineContext*)`, `OnUnload()`,
      `OnUpdate(float)`. Override `OnFixedUpdate/OnRender/OnImGui/OnResize/OnPause/OnResume`
      as needed (they default to no-ops).
- [ ] **`GetModuleInfo()`** — set `name`, `version`, `loadOrder` (lower loads first;
      default 1000). Leave `sdkVersion = SPARK_SDK_VERSION` (the default) so the host's
      `IsSDKCompatible` check passes. `name`/`version` must be module-owned storage —
      string literals are simplest (the engine copies them at registration).
- [ ] **Dependencies (optional)** — declare with `SPARK_MODULE_DEPENDENCIES(info, "OtherModule", ...)`
      inside `GetModuleInfo()` (`ModuleRegistry.h`). ModuleManager topologically sorts
      declared deps; within a level it falls back to `loadOrder`.
- [ ] **Factory exports** — in exactly one `.cpp`, either write them by hand:
      ```cpp
      extern "C" {
        SPARK_MODULE_API Spark::IModule* CreateModule()            { return new YourModule(); }
        SPARK_MODULE_API void            DestroyModule(Spark::IModule* m) { delete m; }
      }
      ```
      or use the macro `SPARK_IMPLEMENT_MODULE(YourModule)` (`ModuleRegistry.h`).
- [ ] **Injection hooks + DllMain** — in that same `.cpp` add:
      ```cpp
      #include <Spark/ModuleDllMain.h>
      ```
      This is **required** on Windows for the module to see the host console, EngineContext,
      and ImGui. Include it in **one** TU only — a second inclusion is a duplicate-`DllMain`
      link error (by design).
- [ ] **CMake** — a `GameModules/<YourModule>/CMakeLists.txt` that builds a `SHARED`
      library and defines `SPARK_MODULE_DLL` (and `SPARK_GAME_DLL` if you also export the
      legacy API). This turns `SPARK_MODULE_API` / `SPARK_GAME_API` into `dllexport`
      (`SparkExport.h`). Link `SparkEngineLib` (Windows) and add `Source`,
      `SparkEngine/Source`, and `SparkSDK/Include` to the include path. The in-tree
      modules copy `GameModules/SparkGameFPS/CMakeLists.txt`; a standalone project uses
      the `spark_add_game_module(<Target> <sources>)` helper in `cmake/SparkGameModule.cmake`.
- [ ] **SDK version** — do not override `info.sdkVersion` unless you know why; a mismatch
      makes `LoadModule` reject the DLL with an "SDK version mismatch" error and unload it.
- [ ] **Legacy path (optional)** — if you also want the old ABI, subclass `IGameModule`
      and export `CreateGameModule` / `DestroyGameModule`; the host wraps it in
      `LegacyModuleAdapter`. New modules should prefer `IModule`.

### Common failure → cause map

| Symptom | Cause | Fix |
|---------|-------|-----|
| `"Module '…' has no recognized exports"` | No `CreateModule`/`CreateGameModule` export | Add factory exports (or `SPARK_IMPLEMENT_MODULE`) and ensure `SPARK_MODULE_DLL` is defined so they export |
| `"SDK version mismatch"` in log, module unloaded | `info.sdkVersion` differs from engine | Leave it at the `SPARK_SDK_VERSION` default; rebuild against current SDK |
| `EngineContext::Get()` returns null inside module | `ModuleDllMain.h` not included → no `SparkModuleInjectEngineContext` export | `#include <Spark/ModuleDllMain.h>` in one `.cpp` |
| Module console commands never appear | Missing `SparkModuleInjectConsole` (same missing include) | same fix |
| Module ImGui blank / crash | Missing `SparkModuleInjectImGui`, or host never called `SetImGuiInjection` | include `ModuleDllMain.h`; host sets injection in `SparkEngineWindows.cpp` game-ImGui layer |
| DLL in output dir but never loaded | Filename lacks `Game`/`Module`/`Plugin`, or matches a system-DLL prefix | rename so the scan picks it up, or load via manifest |

---

## 4. EngineContext-injection P0 — current state (verified)

The `HARDEN_FLEET_HANDOFF.md` doc (root of repo) lists the module EngineContext injection
as **"P0 landed but INERT — finish first"**, needing the DLL-side
`SparkModuleInjectEngineContext` export added and the `NetworkManager` dead fallback removed.

**Re-verified against the code on 2026-07-07 — that handoff note is now stale on the export:**

- **DLL-side export: LANDED.** `SparkModuleInjectEngineContext(void* ctx)` calling
  `EngineContext::SetInjected(...)` **is present** in
  `SparkSDK/Include/Spark/ModuleDllMain.h` (lines ~88–91). It was added in commit
  `52636dda` (2026-07-07, "feat(harden-fleet): Phase 3 deferred cross-cutting fixes").
- **Host side: LANDED.** `ModuleManager::LoadModule` `GetProcAddress`es and calls the
  hook (`ModuleManager.cpp` ~line 191); `EngineContext::Get()` prefers `g_injectedContext`;
  `SetInjected` exists (`EngineContext.{h,cpp}`).
- **All 10 in-tree modules include `<Spark/ModuleDllMain.h>`**, so every module DLL now
  exports the hook. So the injection is **functionally wired end-to-end at the source
  level.** Caveat: the *shipped binaries are stale* — the module DLLs built on disk
  predate the `52636dda` export commit and therefore **lack the export until rebuilt**,
  so the runtime "end-to-end" claim holds only after a rebuild. For the full status,
  the rebuild/verify gates, and the exact binary check, defer to the companion skill
  **`sparkengine-module-injection-p0-campaign`** (the canonical status book) rather than
  re-deriving it here.
- **Still open (cleanup, harmless):** the `static NetworkManager instance;` fallback in
  `NetworkManager::GetInstance()` (`NetworkManager.cpp` ~line 143) has **not** been
  removed. It is now dead in practice — the context branch above it wins once injection
  runs — but the handoff's "remove the dead fallback" line is not yet done.
- **Still open (per handoff):** MMO dependency-injection cleanup — `MMOWorldSetup.cpp`
  still calls `Spark::Streaming::SeamlessAreaManager::GetInstance()` (~lines 135, 230)
  rather than routing through the injected context. Verify before relying on it.

To **finish** the remaining cleanup step by step, use the companion skill
**`sparkengine-module-injection-p0-campaign`** — do not improvise the removal here.

To re-check the P0 state yourself, see the commands in Provenance below.

---

## Provenance and maintenance

Ground-truth files (all read to author this skill, 2026-07-07):
`SparkEngine/Source/Core/ModuleManager.{h,cpp}`, `SparkEngine/Source/Core/EngineContext.{h,cpp}`,
`SparkSDK/Include/Spark/{ModuleDllMain,IModule,ModuleRegistry,SparkExport}.h`,
`SparkEngine/Source/Engine/Networking/NetworkManager.{h,cpp}`,
`GameModules/SparkGameFPS/{CMakeLists.txt,Source/Core/Main.cpp}`,
`cmake/SparkGameModule.cmake`, `CMakeLists.txt` (~line 1397), `HARDEN_FLEET_HANDOFF.md`.

Re-verify the volatile claims (run from repo root, Git Bash):

```bash
# P0 export present? (expect the extern "C" line + SetInjected call)
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h

# Which commit added it / when
git log -1 --format='%h %ci %s' -S SparkModuleInjectEngineContext -- SparkSDK/Include/Spark/ModuleDllMain.h

# Every module still includes the hook header?
grep -rl "ModuleDllMain.h" GameModules

# Is the NetworkManager dead fallback still there? (still open as of 2026-07-07)
grep -n "static NetworkManager instance" SparkEngine/Source/Engine/Networking/NetworkManager.cpp

# MMO GetInstance DI cleanup still pending?
grep -n "SeamlessAreaManager::GetInstance" GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp

# Directory-scan candidate/system-DLL rules unchanged?
grep -n "contains(\"Game\")\|DONT_RESOLVE_DLL_REFERENCES" SparkEngine/Source/Core/ModuleManager.cpp
```

If `ModuleDllMain.h` gains/loses a hook, or the scan rules in `LoadModulesFromDirectory`
change, update sections 1–2. If `NetworkManager::GetInstance()`'s fallback is removed or
the MMO DI cleanup lands, update section 4.
