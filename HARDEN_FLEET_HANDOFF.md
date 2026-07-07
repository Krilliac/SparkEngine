# SparkEngine Hardening Fleet — Handoff (branch `claude/harden-fleet`)

Two commits, both **compile clean (Debug, 0 errors/0 warnings)**:
- `6658a75a` — Phase 1+2: 14-lane audit (130 findings) + 14 disjoint execution buckets. 74 engine files hardened, 30 regression tests added, compile break fixed.
- `762cbb33` — `Tests/` added to `SparkTests` include path (harden tests resolve `TestFramework.h`).

## Validated
- `SparkEngineLib.lib` builds clean with all P0/P1 correctness fixes.
- `SparkTests.exe` builds + links clean incl. all 30 harden tests.

## Known blocker (PRE-EXISTING, not from this branch)
- Full **Debug** suite execution aborts at `TestConstantBufferRing_InitializeNullDeviceFails`
  (`Initialize(nullptr)` hits `Assert::Fail`→`std::abort()` in Debug). Present on `Working`.
  Harden tests register last, so they never run in Debug. To run them: build **Release**
  (asserts are no-ops) or guard/skip the pre-existing null-device assert tests.

## P0 landed but INERT — finish first (trilobite tasks)
1. **Module EngineContext injection** — host side done (`EngineContext::SetInjected` + `Get()` preference + `ModuleManager` `GetProcAddress`). Needs the DLL-side export:
   add `extern "C" __declspec(dllexport) void SparkModuleInjectEngineContext(void*)` in
   `SparkSDK/Include/Spark/ModuleDllMain.h` calling `EngineContext::SetInjected`, AND remove
   `NetworkManager`'s dead per-module `GetInstance()` fallback.

## Deferred cross-cutting P1/P2 (recipes in commit `6658a75a` manifests / agent buildRisks)
- **BT Clone deep-copy** — `BehaviorTreeTypes.h` `BTNode::Clone()=0` + impls per node (composites deep-copy children). Cloned trees currently have no root (silent no-op).
- **WeaponManager ownerEntity** — thread shooter entity through `ProcessWeapon`/`HandleFiring`; `WeaponFireEvent.ownerEntity` is always 0.
- **EditorUI UAF funnel** — add `EditorUI::SwapWorld()` (clear CommandHistory → move world → rewire panels); route `OpenScene()`/init through it.
- **HasUnsavedChanges** — monotonic `m_editSequence`/`m_savedSequence` in `UndoRedoManager.h` (stack-size equality gives false negatives → lost edits).
- **MMO DI** — same `GetInstance()`→context fix still needed in `MMOPlayerSystem.cpp`, `MMOWorldSetup.cpp`.
- **Localization GetString by-value** — `LocalizationSystem.h/.cpp` return `std::string` by value (currently returns ref into map after unlocking → dangling).
- **Sequencer audio wire-in** — add `AudioCallback` typedef + `SetAudioCallback` + dispatch loop; audio cues never fire.
- **ECS** — consolidate `SystemManager`/`PhaseSystemManager`/`StageBasedExecutor`; register systems into `StageBasedExecutor` (dead tick).
- **Tooling** — split `ConsoleApp.cpp` (1092 lines); single-source version string in `SparkConsole/src/main.cpp`.
- **Tests wiring** — 16 orphan `Test*.cpp` never compiled; wire `tools/check-test-registration.sh`; real (non-mock) `AsyncDatabase`/`HotReload`/`CommandParser` tests.

## Cleanup noted
- ~1.4 GB stale `SparkCrash_*.dmp` + loose `Logs*.log`/`ci_*.txt` in repo root (untracked).
