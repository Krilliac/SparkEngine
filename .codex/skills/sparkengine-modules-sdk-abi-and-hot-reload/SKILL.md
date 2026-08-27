---
name: sparkengine-modules-sdk-abi-and-hot-reload
description: >-
  The one home for SparkEngine game-module DLL loading, SDK/ABI contract (including the
  mandatory .sparkabi sidecar and in-image compatibility descriptor), plugin exports,
  cross-DLL injection seams, module enumeration/selection, module and AngelScript script
  hot reload, unload ordering, and module test fixtures. TRIGGER when: "my module DLL won't
  load", "module rejected before OS load", ".sparkabi sidecar", "CreateModule /
  CreateGameModule exports", "SPARK_IMPLEMENT_MODULE", "SDK version mismatch",
  "IsSDKCompatible", "one game module per process / REFUSED to load game module",
  "spark.modules.json", "-game flag", "project selector", "module.reload / module.hotreload
  console commands", "hot reload swallowed my module", "AngelScript .as script hot reload",
  "module commands are dead / EngineContext::Get() is null inside the DLL",
  "SparkModuleInjectEngineContext/Console/ImGui", "crash at exit after a module failed to load",
  "FreeLibrary / dlclose ordering", or writing/registering module tests. DO NOT TRIGGER for:
  shader hot-reload (sparkengine-rendering-rhi-rendergraph-and-shaders), ECS phase ordering /
  engine thread and memory rules (sparkengine-ecs-lifecycle-threading-and-memory), CMake flags
  and compiler toolchain setup (sparkengine-build-ci-and-dependencies), or the history of past
  module incidents (sparkengine-failure-archaeology).
---

# SparkEngine — Modules, SDK/ABI, and Hot Reload

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

Runbook for everything at the engine⇄module-DLL boundary: how a `GameModules/` DLL is found,
loaded, injected, version-checked, updated, hot-reloaded, and torn down — and exactly how much
protection each of those steps really gives you.

## When NOT to use this skill — sibling routing

| You are dealing with… | Use instead |
|---|---|
| AngelScript `.as` script reload, `[server]`/`[client]` contexts | **this skill** — see the script-reload note at the end of §5 |
| Shader (`.hlsl`) hot reload, RHI backends | `sparkengine-rendering-rhi-rendergraph-and-shaders` |
| ECS phase wiring, main-loop update order, threading, allocators | `sparkengine-ecs-lifecycle-threading-and-memory` |
| Compile flags, CMake presets, CI jobs, third-party deps | `sparkengine-build-ci-and-dependencies` |
| "When did this module bug happen and what was tried" | `sparkengine-failure-archaeology` |
| General crash triage that is not module-boundary-specific | `sparkengine-debugging-playbook` |
| Promoting a module change to release | `sparkengine-change-control-and-release-readiness` |

## Vocabulary (defined once)

- **Module DLL** — a `GameModules/SparkGame*/` shared library (`.dll`/`.so`/`.dylib`) that
  statically links `SparkEngineLib`. Because the link is static, each DLL carries its own private
  copy of engine globals (`g_engineContext`, `GImGui`, the console singleton, Jolt registries).
- **Per-image copy** — a global that exists once per loaded binary image. The exe and every DLL
  each have their own; only the image that populated its copy sees a value.
- **Injection hook** — an `extern "C" __declspec(dllexport)` function the host resolves via
  `GetProcAddress` right after `LoadLibrary` and calls to point the DLL's per-image globals at
  host-owned objects. Windows-only (`#if defined(_WIN32)`).
- **SDK version** — `SPARK_SDK_VERSION` (currently **2**) in `SparkSDK/Include/Spark/Version.h`.
  An integer ABI stamp baked into each module at compile time via `ModuleInfo::sdkVersion`.
- **Game vs Addon** — `Spark::ModuleKind` in `SparkSDK/Include/Spark/IModule.h`. Exactly one
  `Game` module per process (game modules own the simulation); `Addon` modules coexist freely.
- **Hot reload** — unload a module DLL and reload the rebuilt file while the engine runs.

## 1. The module contract (implemented, documented)

A module DLL must export, `extern "C"`:

```cpp
SPARK_MODULE_API Spark::IModule* CreateModule();
SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
```

- Generate both with `SPARK_IMPLEMENT_MODULE(MyModuleClass)` from `SparkSDK/Include/Spark/ModuleRegistry.h`.
- Include `SparkSDK/Include/Spark/ModuleDllMain.h` in exactly **one** `.cpp` per DLL (typically
  the same file as `SPARK_IMPLEMENT_MODULE`). It emits the canonical Windows `DllMain`
  (duplicate inclusion = duplicate-symbol link error, by design) **and the three injection
  exports** (§3). No-op on non-Windows.
- Define `SPARK_MODULE_DLL` before including `Spark/SparkExport.h` so `SPARK_MODULE_API`
  expands to `__declspec(dllexport)`. `cmake/SparkGameModule.cmake` sets this and links
  `Spark::SparkEngineLib` for every auto-discovered `GameModules/` target — how the compile
  definitions are applied belongs to `sparkengine-build-ci-and-dependencies`.
- Every module binary must ship with its build-generated `.sparkabi` sidecar
  (`cmake/SparkGameModule.cmake` emits it post-build as `$<TARGET_FILE>.sparkabi`); a module
  without one is invisible to discovery and refused by `LoadModule` before the OS maps it (§2).
- Legacy DLLs exporting `CreateGameModule`/`DestroyGameModule` (returning `IGameModule*`) still
  load, wrapped by `LegacyModuleAdapter` in `SparkEngine/Source/Core/ModuleManager.cpp` (~line 63).
- `ModuleInfo` string fields (`name`, `version`) must point at module-owned storage valid until
  `OnUnload()` — string literals recommended (`Spark/IModule.h` header comment).
- Dependencies: `SPARK_MODULE_DEPENDENCIES(info, "OtherModule", ...)` → `ModuleManager::SortModules()`
  runs Kahn's topological sort, falling back to `loadOrder` (lower loads first, default 1000);
  circular deps are logged as errors and appended anyway (`ModuleManager.cpp` ~line 891).
- Lifecycle order the engine drives: `OnLoad → OnUpdate/OnFixedUpdate/OnRender/OnImGui per frame
  → OnResize/OnPause/OnResume as events → OnUnload` (reverse load order at shutdown). Where in the
  frame these land is `sparkengine-ecs-lifecycle-threading-and-memory` territory.
- Full boundary rules (memory ownership, thread safety, no STL types across the ABI edge for
  exports): `docs/specs/plugin-abi-guide.md` — this skill does not contradict it.

## 2. The compatibility gate: mandatory `.sparkabi` sidecar + in-image descriptor (current truth)

The old "gate runs late" problem is fixed in the current tree. The gate now fires **before
the OS loader maps the image**, in two layers (verified in
`SparkEngine/Source/Core/ModuleManager.cpp::LoadModule`):

| Step | Runs module code? |
|---|---|
| 1. Path-traversal check (rejects `..`) | no |
| 2. **Mandatory `.sparkabi` sidecar validation** (`ValidateModuleSidecar`) — a `<module>.dll.sparkabi` key=value file with exactly 12 fields (struct size, magic, format, SDK version, runtime ABI version, compiler family/ABI, C++ language level, runtime library, iterator debug level, pointer size, `binary_sha256`). The descriptor is checked via `Spark::CheckModuleCompatibility`, then the module binary is hashed and must match `binary_sha256`. Missing, malformed, incompatible, or hash-mismatched sidecar ⇒ "`Module '<path>' rejected before OS load`" — **before `LoadLibrary`/`dlopen`, before DllMain or any static constructor** | **no** |
| 3. `LoadLibraryA` / `dlopen(RTLD_NOW)` | yes — DllMain + static initializers (only after step 2 passed) |
| 4. **Mandatory in-image C descriptor** — `SPARK_MODULE_COMPATIBILITY_EXPORT_NAME` resolved and re-checked (`CheckModuleCompatibility`) as defense in depth. It has a fixed C ABI and returns an integer-only POD; no C++ object, allocator, vtable, or engine pointer crosses before both checks pass. A missing export ⇒ "rejected before injection/factory" + unload | integer-only C call |
| 5. Injection hooks: console, EngineContext (+ Jolt `EnsureImageRuntime`), ImGui | yes |
| 6. `CreateModule()` → `GetModuleInfo()` → `IsSDKCompatible(info.sdkVersion)` (exact-equality vs `SPARK_SDK_VERSION`, `Version.h`) — now a redundant final stale-build check | yes |
| 7. Single-game-module policy check | — |

Consequences (all verified in source, 2026-08-23):

- The sidecar's **SHA-256 binds the descriptor to the exact binary**, so a stale or
  swapped DLL is refused before any of its code runs. `cmake/SparkGameModule.cmake`
  generates the sidecar as a post-build step for every auto-discovered module target.
- The sidecar gate covers **all** load paths, including legacy `CreateGameModule` DLLs
  (whose `LegacyModuleAdapter` still stamps the engine's own `SPARK_SDK_VERSION` into
  `GetModuleInfo()` — the step-6 check is trivially satisfied for them, but steps 2/4
  gate them for real now).
- **Discovery is non-executing.** `DiscoverModuleCandidates` filters by filename hints
  (contains `Game`/`Module`/`Plugin`; excludes `d3d*`, `vcruntime*`, `msvcp*`,
  `ucrtbase*`, `*SparkConsole*`, `*SparkEngine*`) **plus mandatory sidecar presence** —
  it never maps an image, not even with platform probe flags, on any OS.
  `DiscoverModules` (used by the editor's `GameModuleSelectorPanel`) builds on that and
  intentionally presents **filename/"unknown"** metadata for unloaded modules; version
  and module-kind become known only once a module is actually loaded (or a manifest
  supplies trusted metadata). No editor probe executes candidate DLL code anymore.
- Residual honest caveat: the sidecar is an **integrity/compatibility gate, not an
  authentication boundary** — anyone who can drop a DLL next to the engine can also
  regenerate a matching sidecar. "Only put trusted, same-toolchain DLLs where the engine
  scans" (per `docs/specs/plugin-abi-guide.md`) still applies. Bumping
  `SPARK_SDK_VERSION` on any binary-incompatible SDK change is still mandatory.

Regression coverage: `Tests/TestModuleABI.cpp` (registered in `Tests/CMakeLists.txt`)
asserts discovery does not execute candidates, mismatches are rejected before static
constructors/injection/factory, a modified binary is rejected by hash before
DllMain, and a compatible `SPARK_IMPLEMENT_MODULE` module still loads.

## 3. Injection seams (Windows-only, implemented, wired)

After `LoadLibrary` and the mandatory in-image compatibility check — but before
`CreateModule` — the host probes and calls three exports, all defined in
`SparkSDK/Include/Spark/ModuleDllMain.h` (each is a silent no-op skip if absent):

| Export | Fixes | Extra behavior |
|---|---|---|
| `SparkModuleInjectConsole` | Module console-command registrations landing in a DLL-private `SimpleConsole` (commands silently dead) | — |
| `SparkModuleInjectEngineContext` | `EngineContext::Get()` returning null inside the DLL (per-image `g_engineContext`) | Also calls `PhysicsSystem::EnsureImageRuntime()` — registers the module image's Jolt allocator/Factory/type tables. Pointer is stored **non-owning**; module teardown never frees the host context |
| `SparkModuleInjectImGui` | Per-image `GImGui` + allocators — module `OnImGui()` drawing nothing or crashing | Payload staged by `ModuleManager::SetImGuiInjection`, set in `SparkEngineWindowsInit.cpp` (~line 241) before modules load |

Rules that must not regress:

- The `GetProcAddress` string literals in `ModuleManager.cpp` (~lines 179, 193, 205) must stay
  **byte-identical** to the export names in `ModuleDllMain.h`. A typo degrades to a silent no-op,
  not a crash.
- Never reach a subsystem from module code via `Something::GetInstance()` — statically-linked
  singletons are per-image; use `m_context->GetX()` / `EngineContext::Get()->GetX()`.
- Do not port the seam to Linux/macOS as-is; the per-image-global problem is specific to
  statically linking each DLL on Windows.

## 4. Enumeration and selection (implemented, wired)

Load priority in `LoadGameModules` (`SparkEngine/Source/Core/SparkEngineWindowsModules.cpp`):

1. **`-game <path>`** on the command line — loads that single DLL.
2. **`spark.modules.json`** next to the exe → `LoadModulesFromManifest`. The "parser" is a
   naive substring scan for `"path"` keys (`ModuleManager.cpp` ~lines 407–445) — it is not a real
   JSON parser; relative paths resolve against the manifest directory; keep the format minimal.
3. **Bare launch** → `DiscoverModuleCandidates` (no loading). One candidate: load it. Several:
   arm the ImGui project selector; the pick is consumed **in the main loop, next frame**
   (`ConsumeProjectSelectorChoice`) — never load a DLL from inside the ImGui render callback.
   "Remember choice" writes a minimal `spark.modules.json` (delete it to get the selector back).
4. The old load-everything directory scan is gone as the default; `LoadModulesFromDirectory`
   still exists for addon packs, and the **single-game-module policy** hard-refuses a second
   `ModuleKind::Game` module either way ("REFUSED to load game module" in the log). Libraries
   and extensions must declare `ModuleKind::Addon` in their `ModuleInfo`.

## 5. Hot reload — two systems exist; only one is load-bearing

### 5a. The wired one: `Spark::ModuleHotReloadManager` (`SparkEngine/Source/Core/ModuleHotReload.{h,cpp}`)

- Owned by `EngineRuntime::moduleHotReload`; created + `Initialize` + `WatchAllLoadedModules` +
  `Start` in `SparkEngineWindowsInit.cpp:190–193` (also headless and Linux init); `PollChanges()`
  called each frame from the platform main loops (`SparkEngineWindowsWin32.cpp:175–176`, both
  headless loops, `SparkEngineLinuxInit.cpp:149–150`).
- Polls file mtime+size; 500 ms debounce (`SetDebounceMs`); one retry after 200 ms if the reload
  fails (file possibly still locked by the linker).
- Console commands (registered in `EngineConsoleCommands.cpp` ~700–745):
  `module.hotreload.enable`, `module.hotreload.disable`, `module.hotreload.status`,
  `module.reload <name>` (force).

### 5b. The unwired one: `Spark::HotReload::ModuleHotReload` singleton (`SparkEngine/Source/Engine/HotReload/ModuleHotReload.h`)

Header-only, shadow-copy design. `GameplayLifecycleShared.cpp` calls its `Initialize()`
(~line 519), `Update(dt)` every frame (~line 1124), and `Shutdown()` (~line 1269) — but **nothing
in the tree ever calls `RegisterModule` or `SetCallbacks` on it** (verified by repo-wide grep).
It watches zero modules and its unload/reload callbacks are empty: an initialized, updated,
unit-tested **no-op**. Under the project's "wire it in or delete it" rule this is an `open`
consolidation candidate — either give it the ModuleManager callbacks (gaining its shadow-copy
and cooldown features) or delete it and its tests. Do not "fix" hot reload by tuning this class;
the behavior you observe at runtime comes from 5a.

### 5c. Current hot-reload risks (verified in source, 2026-08-23)

- **Watcher reentrancy is fixed.** `PollChanges()` now holds `m_mutex` only while detecting and
  moving debounce-ready records. Reload work, the 200 ms retry wait, module hooks, logging, and
  the user callback run after that lock is released. `ForceReload()` likewise copies the callback
  under lock and invokes it outside. `ModuleABI_HotReloadCallbackCanReenterManagerWithoutDeadlock`
  proves a callback can call `GetStatus()` and replace itself. Keep callbacks bounded anyway:
  polling still occurs on the main thread, so the retry `sleep_for(200ms)` remains a visible stall.
- **Replacement is transactionally shadow-staged.** `ModuleManager::ReloadModule` copies the DLL
  and `.sparkabi` to a unique `.spark-reload-<token>-<serial>` image, loads it through the normal
  sidecar/hash/descriptor gates, checks identity and the one-game-module policy, and runs
  `InitializeAll(context)` while the working module remains loaded. Validation/load/`OnLoad`
  failure cleans up the staged entry and files and preserves the old instance **and its state**.
  Only a fully initialized replacement commits: old `OnUnload`, `UnloadEntry`, swap, sort.
  `ModuleABI_FailedTransactionalReloadPreservesWorkingModule` and
  `ModuleABI_FailedReplacementInitializationPreservesWorkingModule` cover both failure classes.
- **Residual staging-side-effect caveat (not covered by the fixtures).** Replacement `OnLoad`
  runs before old `OnUnload`, so for a short staging interval both instances are initialized
  against the same `IEngineContext`. A module that registers process-global names/callbacks must
  make registration failure-safe and ensure old `OnUnload` cannot erase the replacement's newly
  registered state. The fixture modules do not exercise such shared side effects; verify live for
  modules that own console/event/network registrations.
- **Windows first-rebuild caveat remains inferred.** A committed replacement runs from its shadow
  image, freeing the original path for later linker writes, and `UnloadEntry` removes obsolete
  transient images after closing them. But the initial process load still maps the original DLL;
  the first linker overwrite may therefore be blocked until a force reload has moved the active
  instance to a shadow. This is static reasoning, not a measured live result.

**AngelScript `.as` script hot reload (owned here, distinct from module DLL reload).**
Script-level reload lives in `SparkEngine/Source/Engine/Scripting/` (ScriptSystem), not in
the module managers above. Its load-bearing behavior: `ReloadScript` pre-validates the new
source by compiling into a throwaway staging module (`moduleName + "$hotreload_stage"`)
*before* detaching live `asIScriptObject` instances, so a typo cannot dangle every live
script of that module (hardened in commit `0d3cbcbf` — story in
`sparkengine-failure-archaeology`, Era 2). Per-instance script state is **not** preserved
across a reload, by design. Server/client script contexts are kept separate by the
scripting subsystem; grep `Engine/Scripting` for the current context split before relying
on details here.

## 6. Unload ordering — the contract at shutdown

Reverse-load-order `OnUnload` via `ShutdownAll`, then a strict pre-`FreeLibrary` teardown in
`ShutdownEngine` (`SparkEngine/Source/Core/SparkEngine.cpp` ~lines 283–336). The invariant:
**anything whose vtable, lambda, or deleter lives in module code must be destroyed while the
module image is still mapped.** Verified sequence:

1. `moduleManager->ShutdownAll()` — modules' `OnUnload` in reverse order.
2. `SimpleConsole::Shutdown()` + `ConsoleProcessManager::Shutdown()` — command handlers may be
   module lambdas.
3. `eventBus->ClearAll()` — `ChannelOf<E>` vtables live in the `.so`/`.dll`.
4. `NetworkManager::GetInstance().ClearHandlers()` — net handlers are module lambdas.
5. `g_engineEcsWorld.reset()` — entt pools carry module-code deleters.
6. `ShutdownPhysics()` — Jolt bodies/shapes created by module code have module-image vtables.
7. Only then `UnloadAll()` (`FreeLibrary`/`dlclose`) — **except** Linux/headless, which
   deliberately `rt.moduleManager.release()`s and keeps modules mapped until process exit to
   dodge a late-shutdown crash path (`open` workaround, commented in source).

The main loop resets `moduleHotReload` *before* this sequence (`SparkEngineWindowsWin32.cpp:203`),
so no reload can race the teardown. If you add any new sink that stores module-provided callables
(event handlers, coroutines, timers), it must be cleared between steps 1 and 7 — put it next to
the existing clears and say why in a comment.

## 7. Module test fixtures — what is and is not covered

| Test file (all registered in `Tests/CMakeLists.txt`) | What it tests | Honest scope |
|---|---|---|
| `Tests/TestModuleABI.cpp` (8 tests) | The `.sparkabi`/descriptor gate against **real fixture module DLLs through the real `ModuleManager`** plus the wired reload path: discovery does not execute candidates; mismatch/hash failures are pre-load; compatible macro module loads; validation and replacement-`OnLoad` failures preserve the working instance/state; watcher callback can reenter without deadlock | Production `ModuleManager` + built fixture DLLs (`SPARK_TEST_MISMATCHED_MODULE_PATH` / `SPARK_TEST_COMPATIBLE_MODULE_PATH`) + production `ModuleHotReloadManager` |
| `Tests/TestModuleDependency.cpp` (5 tests) | Topological sort / load order | **Standalone mirror** of `SortModules` logic — does not execute `ModuleManager` |
| `Tests/TestModuleDiscovery.cpp` (6 tests) | Manifest generation, multi-game detection | Standalone mirror structs — no DLL, no `ModuleManager` |
| `Tests/TestModuleHotReload.cpp` (11 tests) | The **unwired 5b singleton's** registration/status API | Never performs a real reload; does not touch the wired 5a manager |
| `Tests/TestPhysicsTeardownGuard.cpp` | Physics teardown guard behavior | Related to §6 step 6 |

`TestModuleABI.cpp` closes the old "no unit test loads a real module DLL" gap for the
**compatibility gate, load/refusal paths, and transactional reload failure/reentrancy paths**.
Still exercised only by live engine runs: the injection seams' end-to-end behavior
(console/ImGui payloads), the project selector, successful rebuild-while-running with real
module side effects, and the Windows first-rebuild file-lock behavior. When claiming
"module loading works", say which evidence you
have — the standalone-mirror tests passing is *not* it (a mirror can pass while the real
code diverges), and no full-suite/CI run has been captured at this exact working tree.

## 8. Status ledger (distinguish these; do not blur them)

| Capability | Implemented | Tested | CI-enforced | Release-ready |
|---|---|---|---|---|
| CreateModule/DestroyModule + legacy adapter load path | yes | real-`ModuleManager` load/refusal paths in `TestModuleABI.cpp`; rest live-run only | registered in SparkTests (runs under the blocking ctest jobs once merged; no full-suite evidence captured at this exact tree) | yes for trusted DLLs |
| `.sparkabi` sidecar + in-image descriptor gate (pre-OS-load, SHA-256-bound) | yes | `TestModuleABI.cpp` (production `ModuleManager` + fixture DLLs) | registered in SparkTests (same caveat) | strong compatibility gate; **not an authentication boundary** (§2) |
| SDK version gate (`IsSDKCompatible`, exact match, v2) | yes — now a redundant final check behind the sidecar/descriptor gate | indirectly via `TestModuleABI.cpp` | same | see §2 |
| Injection seams (console/context/ImGui) | yes (Win32) | live-run only | no | yes on Windows |
| Single-game-module policy + Kind::Addon | yes | mirror test only | no | yes |
| Dependency toposort | yes | mirror test only | no | yes |
| Manifest / `-game` / project selector | yes | no | no | yes |
| Hot reload (wired 5a) | yes | 3 production-path tests in `TestModuleABI.cpp` (two rollback/preservation cases + callback reentrancy); no real rebuild/live side-effect fixture | registered in SparkTests; no full-suite/CI evidence captured at this exact tree | **candidate** — transactional core verified; staging-side-effect + Windows first-rebuild caveats remain (§5c) |
| Shadow-copy hot reload (5b) | built, **not wired** | API-only tests | no | no — consolidation candidate |
| Unload ordering contract | yes | teardown-guard test partial | no | yes on Windows; Linux/headless leaks modules by design (`open`) |

## 9. Failure modes → first moves

| Symptom | Likely cause | First command |
|---|---|---|
| "Module '<path>' rejected before OS load: missing mandatory ABI sidecar" | No `.sparkabi` next to the DLL (hand-copied binary, or built outside `spark_add_game_module`) | Copy/regenerate the sidecar with the DLL; check `ls <module>.dll.sparkabi` |
| "Module '<path>' rejected before OS load: ABI sidecar binary hash mismatch" | DLL replaced/modified after its sidecar was generated (stale or tampered pair) | Rebuild the module so binary + sidecar match |
| "rejected before injection/factory: missing mandatory … export" | Module built with an SDK too old to emit the compatibility descriptor | Rebuild with the current SDK (`SPARK_IMPLEMENT_MODULE` exports it automatically) |
| "Module '<path>' has no recognized exports" | Missing `SPARK_IMPLEMENT_MODULE` / `SPARK_MODULE_DLL` not defined | `grep -rn "SPARK_IMPLEMENT_MODULE" GameModules/<Mod>/Source/` |
| "SDK version mismatch (module=1, engine=2)" | Module built against stale SDK headers | Rebuild the module; check `grep -n "SPARK_SDK_VERSION" SparkSDK/Include/Spark/Version.h` |
| Module doesn't appear in discovery/selector at all | Sidecar missing (discovery requires it), or filename lacks `Game`/`Module`/`Plugin` | `ls <dir>/*.sparkabi`; check the filename hints in `DiscoverModuleCandidates` |
| "REFUSED to load game module" | Second `ModuleKind::Game` in scan/manifest | Mark libraries `ModuleKind::Addon`, or pick one game module |
| Module console commands do nothing (Windows) | `SparkModuleInjectConsole` export missing → per-image console | `grep -c SparkModuleInjectConsole <built module>.dll` (0 = stale/missing) |
| `EngineContext::Get()` null / wrong singleton in module | Injection export missing or stale binary | `grep -acE SparkModuleInjectEngineContext <built module>.dll` |
| Module ImGui draws nothing / crashes | ImGui injection missing, or module built without `SPARK_HAS_IMGUI` | Check `SparkModuleInjectImGui` export + `SetImGuiInjection` call in init |
| Engine hangs during hot reload | Long module hook/callback, the intentional 200 ms retry, or a subsystem lock cycle; watcher-callback self-reentry is regression-tested safe (§5c) | `module.hotreload.disable`; inspect hook/callback duration and cross-subsystem locks |
| Module vanished after a failed reload | Regression of transactional staging — validation/`OnLoad` failure must preserve the old instance (§5c) | Run `SPARK_TEST_NAME=ModuleABI_Failed ./build/bin/SparkTests`; inspect `ReloadModule` commit ordering |
| AV at process exit after a module failed OnLoad | Regression of the immediate-destroy cleanup (§5c) | `grep -n "failed OnLoad" SparkEngine/Source/Core/ModuleManager.cpp` |
| Segfault at shutdown in console/event/net teardown | New module-callable sink cleared after `FreeLibrary` (§6) | Read `ShutdownEngine` ordering in `SparkEngine/Source/Core/SparkEngine.cpp` |
| Editor selector shows "unknown" version/kind for a module | Expected — discovery is non-executing; unloaded modules keep filename/"unknown" metadata (§2) | Load the module (or supply a manifest) to get real metadata |

## Provenance and maintenance

All claims verified against the working tree on **2026-08-23** (branch
`claude/whole-nine-yards-20260823`) by reading the cited files; no builds or live runs were
performed for this skill — items marked "live-run only" or "inferred" reflect that. Line numbers
are approximate anchors; re-verify with:

```bash
# Gate ordering: sidecar → LoadLibrary → in-image descriptor → injections → CreateModule → IsSDKCompatible
grep -n "ValidateModuleSidecar\|rejected before OS load\|SPARK_MODULE_COMPATIBILITY_EXPORT_NAME\|LoadLibraryA\|SparkModuleInject\|IsSDKCompatible" SparkEngine/Source/Core/ModuleManager.cpp
# Sidecar SHA-256 binding + field schema
grep -n "binary_sha256\|ComputeModuleSha256" SparkEngine/Source/Core/ModuleManager.cpp
# Discovery stays non-executing and sidecar-gated (expect the sparkabi presence check, no image mapping)
grep -n "sparkabi\|never maps" SparkEngine/Source/Core/ModuleManager.cpp
# Sidecar generation in the module build helper
grep -n "sparkabi" cmake/SparkGameModule.cmake
# Real-ModuleManager ABI regression tests registered
grep -n "TestModuleABI" Tests/CMakeLists.txt
# SDK version constant and exact-match check
grep -n "SPARK_SDK_VERSION\|IsSDKCompatible" SparkSDK/Include/Spark/Version.h
# Injection exports still match host lookup strings byte-for-byte
grep -n "SparkModuleInject" SparkSDK/Include/Spark/ModuleDllMain.h SparkEngine/Source/Core/ModuleManager.cpp
# Wired watcher: reload/retry/callback must remain outside m_mutex
grep -n "lock_guard\|m_reloadCallback\|sleep_for\|ReloadModule" SparkEngine/Source/Core/ModuleHotReload.cpp
# Unwired singleton: expect NO RegisterModule/SetCallbacks call sites outside tests
grep -rn "GetInstance().RegisterModule\|GetInstance().SetCallbacks" SparkEngine/Source Tests
# Transactional shadow staging + failed-OnLoad cleanup + commit
grep -n "spark-reload\|preserving module\|Commit only\|failed OnLoad" SparkEngine/Source/Core/ModuleManager.cpp
# Shutdown ordering contract
grep -n "ShutdownAll\|ClearHandlers\|ClearAll\|UnloadAll\|release()" SparkEngine/Source/Core/SparkEngine.cpp
# Module tests registered
grep -n "TestModule" Tests/CMakeLists.txt
```
