# Test Suite Audit — Deep Coverage Analysis

**Last updated:** 2026-04-02
**Type:** Observation
**Status:** Active
**Severity:** Medium (236 test files / 3,076 tests exist, but some engine headers + 105 editor headers + 88 game module headers have no dedicated tests)

## Executive Summary

| Metric | Value |
|--------|-------|
| Test files | 236 |
| Total TEST/TEST_F macros | 3,076 |
| Engine headers (SparkEngine/Source) | 405 |
| Engine headers with dedicated test | 211 (52%) |
| Engine headers with NO test | 194 (48%) |
| Editor headers (SparkEditor/Source) | 109 |
| Editor headers with any test reference | 4 (3.7%) |
| GameModule headers | 102 |
| GameModule headers with any test reference | 14 (13.7%) |
| SparkConsole/SparkShaderCompiler | 0 tests |
| Test files with ≤5 tests (thin coverage) | 41 files |

## Coverage by Engine Subsystem

### Untested Headers by Directory (194 total)

| Directory | Untested Count | Total Headers | Coverage |
|-----------|---------------|---------------|----------|
| Graphics | 74 | ~96 | ~23% |
| Utils | 24 | ~48 | ~50% |
| Core | 24 | ~30 | ~20% |
| Engine/ECS/Components | 14 | ~20 | ~30% |
| Graphics/RHI | 12 | ~18 | ~33% |
| Engine/Networking | 10 | ~14 | ~29% |
| Engine/AI | 10 | ~16 | ~38% |
| Physics | 9 | ~13 | ~31% |
| Engine/ECS/Systems | 7 | ~10 | ~30% |
| Engine/Animation | 6 | ~10 | ~40% |
| Graphics/HybridRT | 5 | ~6 | ~17% |
| Game | 5 | ~6 | ~17% |
| Audio | 4 | ~6 | ~33% |
| Input | 3 | ~5 | ~40% |
| Engine/Scripting | 3 | ~5 | ~40% |
| Others (16 dirs) | ~18 | ~22 | varies |

## CRITICAL: Missing Test Suites (Priority 1)

These are large, complex systems with **zero dedicated tests** that sit on critical execution paths:

### 1. NetworkManager (651 lines, 30+ states)
- **File:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`
- **Why critical:** Multiplayer foundation. Connection state machines, lag compensation, hitbox rewinding, reliable message retransmission, RTT estimation.
- **Testable without hardware:** Message serialization, sequence number tracking, ACK handling, duplicate detection, reconnect logic, bandwidth tracking.
- **Existing partial coverage:** `TestNetBuffer`, `TestReliableChannel`, `TestNetworkEncryption` cover components — but NO orchestration test for NetworkManager itself.

### 2. ECSystems (847 lines, 10+ systems)
- **File:** `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h`
- **Why critical:** Core game loop (physics → animation → AI → audio → render). System ordering bugs cascade through entire engine.
- **Testable without hardware:** System registration, execution ordering, entity query filtering, lifecycle callbacks.
- **Existing partial coverage:** `TestECSWorld`, `TestECSIntegration` cover ECS primitives — but NO test for system ordering or SystemManager orchestration.

### 3. MaterialSystem (532 lines)
- **File:** `SparkEngine/Source/Graphics/MaterialSystem.h`
- **Why critical:** PBR rendering pipeline. Invalid material states cause visual artifacts.
- **Testable without hardware:** PBR property validation (metallic/roughness [0,1]), material instancing, variant management, serialization.
- **Existing partial coverage:** `TestMaterialDefinition`, `TestMaterialEffects` — but NO MaterialSystem orchestration test.

### 4. AssetPipeline (496 lines)
- **File:** `SparkEngine/Source/Graphics/AssetPipeline.h`
- **Why critical:** Streaming memory management. LRU cache eviction, async loading, memory budget.
- **Testable without hardware:** LRU algorithm, request queue ordering, hot reload detection, memory accounting.
- **Existing partial coverage:** None.

### 5. EngineSettings (598 lines, 47 public methods)
- **File:** `SparkEngine/Source/Core/EngineSettings.h`
- **Why critical:** Centralizes all subsystem configuration. Parser bugs break engine startup.
- **Testable without hardware:** INI parsing, type conversion, default values, change notifications, validation ranges.
- **Existing partial coverage:** None.

## HIGH: Missing Test Suites (Priority 2)

### 6. AISystem (415 lines)
- **File:** `SparkEngine/Source/Engine/AI/AISystem.h`
- **Testable:** Behavior tree template cloning, handle assignment, blackboard updates.
- **Existing partial:** `TestAIBehaviorTree`, `TestBehaviorTreeNodes` — but no AISystem orchestration.

### 7. PerceptionSystem (478 lines)
- **File:** `SparkEngine/Source/Engine/AI/PerceptionSystem.h`
- **Testable:** FOV cone math, hearing radius, memory decay, octree spatial queries.
- **Existing partial:** `TestEnvironmentQuery` — but no vision/hearing/memory tests.

### 8. InputManager (474 lines)
- **File:** `SparkEngine/Source/Input/InputManager.h`
- **Testable:** Key state edge detection, mouse delta with dead zone, sensitivity.
- **Existing partial:** `TestInputBindings`, `TestInputSystem`, `TestSubTickInput` — but no InputManager state machine test.

### 9. GamepadInput (484 lines)
- **File:** `SparkEngine/Source/Input/GamepadInput.h`
- **Testable:** Dead zone processing (circular vs axial), stick normalization, trigger thresholds, vibration timers.
- **Existing partial:** None.

### 10. WorldServer / AreaServer
- **Files:** `SparkEngine/Source/Engine/Networking/WorldServer.h`, `AreaServer.h`
- **Testable:** Server-side area management, player routing, instance creation.
- **Existing partial:** `TestDedicatedServer` covers basic server setup but not multi-area.

### 11. SparkEngineCamera (404 lines)
- **File:** `SparkEngine/Source/Camera/SparkEngineCamera.h`
- **Testable:** Pitch clamping, matrix updates, zoom transitions, aspect ratio handling.
- **Existing partial:** `TestCameraInterpolation` — but no camera transform/matrix tests.

## MEDIUM: Missing Test Suites (Priority 3)

| System | File | Lines | Key Testable API |
|--------|------|-------|-----------------|
| ~~AudioMixer~~ | ~~Audio/AudioMixer.h~~ | ~~331~~ | ~~Bus hierarchy, volume cascading, reverb blending~~ — **Resolved** (TestAudioMixerBus.cpp) |
| ~~MusicManager~~ | ~~Audio/MusicManager.h~~ | ~~306~~ | ~~Crossfade timing, playlist sequencing~~ — **Resolved** (TestMusicManager.cpp, 24 tests) |
| ~~AngelScriptEngine~~ | ~~Engine/Scripting/AngelScriptEngine.h~~ | ~~402~~ | ~~Compilation errors, hot reload, module tracking~~ — **Resolved** (TestAngelScriptEngine.cpp, 14 tests) |
| ~~ScriptHotReload~~ | ~~Engine/Scripting/ScriptHotReload.h~~ | ~~335~~ | ~~File watching, reload triggers, state persistence~~ — **Resolved** (TestScriptHotReload.cpp, 16 tests) |
| ~~ConsoleVariable~~ | ~~Utils/ConsoleVariable.h~~ | ~~457~~ | ~~CVar registration, type parsing, change callbacks~~ — **Resolved** (TestConsoleVariables.cpp) |
| ~~VRSystem~~ | ~~Engine/VR/VRSystem.h~~ | ~~186~~ | ~~Tracking config, eye matrix, haptic timers~~ — **Resolved** (TestVRSystem.cpp, 12 tests) |
| ~~WeaponManager~~ | ~~Engine/Gameplay/WeaponManager.h~~ | ~~307~~ | ~~Fire rate, spread calc, recoil recovery~~ — **Resolved** (TestWeaponMechanics.cpp) |
| ~~GameObject~~ | ~~Game/GameObject.h~~ | ~~320~~ | ~~World matrix, direction vectors, transform chain~~ — **Resolved** (TestGameObjectTransforms.cpp) |
| ~~SeamlessAreaManager~~ | ~~Engine/Streaming/SeamlessAreaManager.h~~ | ~~—~~ | ~~Area transitions, origin rebasing triggers~~ — **Resolved** (TestSeamlessAreaManager.cpp, 12 tests) |

## LOW: Untested But Acceptable

These are either:
- **Platform stubs** (`PlatformDirectXMathStubs`, `PlatformD3DStubs`, `PlatformAudioStubs`) — tested implicitly
- **RHI backend implementations** (`D3D11Device`, `D3D12Device`, `VulkanDevice`, `MetalDevice`, `OpenGLDevice`) — require hardware
- **Type/enum headers** (`AnimationTypes`, `PhysicsTypes`, `RHITypes`, `GraphicsEnums`) — data-only
- **Internal helpers** (`MakeDesc`, `DrawSortKey`, `LogMacros`) — trivial
- **Debug tools** (`CpuDebugger`, `ThreadDebugger`, `CacheDebugger`, `IODebugger`) — diagnostic only

## Test Quality Issues (Existing Tests)

### Shallow Test Files (tests exist but coverage is superficial)

| File | Tests | Problem |
|------|-------|---------|
| TestGraphicsEngine.cpp | 13 | Only tests struct defaults and enum distinctness — NO actual rendering logic |
| TestModSystem.cpp | 5 | Only tests empty states and error cases — NO actual mod loading |
| TestConstantBufferDiff.cpp | 2 | Only basic diffing — missing perf, large data |
| TestGPUPerfCounters.cpp | 2 | Only accumulation — missing cross-category |
| TestMultiISADispatch.cpp | 2 | Only selection — missing actual dispatch |
| TestWorkSema.cpp | 2 | Only basic — missing stress/contention |

### Thin Test Files (≤5 tests, but tests are meaningful)

41 files have 5 or fewer tests. Most are **meaningful** (testing core functionality) but need expansion:
- `TestClothSimulation` (4) — missing constraints, wind, stability
- `TestDialogueSystem` (4) — missing branching choices, callbacks
- `TestLoadingScreen` (4) — missing task dependencies, timeouts
- `TestClientPrediction` (5) — missing jitter/lag, rollback accuracy
- `TestInputBindings` (5) — missing persistence, complex combos
- `TestShaderGraphCompiler` (5) — missing code generation validation

## Editor Coverage Gap (109 headers, ~4% tested)

The SparkEditor has 109 headers across 52 panels with virtually no test coverage:

**Testable editor systems (no GPU needed):**
- `UndoRedoManager` — command stack, redo chain, undo limits
- `CommandHistory` — partial (TestCommandHistory exists but only tests engine-side)
- `CommandPalette` — fuzzy search, command matching
- `AssetDatabase` — metadata indexing, file scanning
- `PrefabManager` — prefab instantiation, overrides, nesting
- `SceneSerializer` — serialization round-trips
- `EditorLayoutManager` — layout save/restore
- `VersionControlSystem` — status tracking, diff calculation
- `LevelStreamingSystem` — level loading/unloading state machine
- `GizmoSystem` — transform gizmo math (testable without rendering)

## Game Module Coverage Gap (102 headers, ~14% tested)

7 game modules with 102 headers, very few tested:

| Module | Headers | Tests |
|--------|---------|-------|
| SparkGame (FPS) | ~25 | TestFPSComponents, partial |
| SparkGameMMO | ~15 | TestNetworkMMOIntegration only |
| SparkGameRPG | ~8 | None |
| SparkGameARPG | ~8 | None |
| SparkGameRTS | ~8 | None |
| SparkGameRacing | ~8 | None |
| SparkGamePlatformer | ~8 | None |

## Recommended Test Addition Priority

### Phase 1 — Critical Systems (est. 15 new test files, ~400 tests)
1. `TestNetworkManagerOrchestration` — connection state machine, message routing, lag compensation
2. `TestECSystemOrdering` — system execution order, manager lifecycle
3. `TestMaterialSystemValidation` — PBR validation, instancing, serialization
4. `TestAssetPipelineCache` — LRU eviction, async loading, memory budget
5. `TestEngineSettingsParser` — INI parsing, type conversion, defaults, callbacks
6. `TestPerceptionSystemMath` — FOV cones, hearing radius, memory decay, octree
7. `TestInputManagerState` — edge detection, mouse delta, dead zone
8. `TestGamepadInputProcessing` — stick normalization, dead zone, trigger thresholds
9. `TestCameraTransforms` — pitch clamping, matrix updates, zoom
10. `TestWorldServerRouting` — area management, player routing

### Phase 2 — High-Value Expansion (est. 10 new test files, ~200 tests)
11. `TestAudioMixerBus` — bus hierarchy, volume cascading, reverb
12. `TestMusicManagerPlayback` — crossfade, playlists, combat intensity
13. `TestAngelScriptCompilation` — compile errors, hot reload, module tracking
14. `TestConsoleVariables` — CVar registration, type parsing, callbacks
15. `TestWeaponMechanics` — fire rate, spread, recoil
16. `TestGameObjectTransforms` — world matrix, direction vectors
17. `TestUndoRedoManager` — command stack, redo chain (editor)
18. `TestPrefabManager` — instantiation, overrides (editor)
19. `TestSceneSerializer` — round-trip serialization (editor)
20. `TestGizmoMath` — transform gizmo calculations (editor)

### Phase 3 — Game Module Tests (est. 7 new test files, ~100 tests)
21-27. One test file per game module for core mechanics (RPG combat, RTS resources, Racing physics, etc.)

### Phase 4 — Deepen Existing Shallow Tests (est. ~150 additional tests)
- Expand TestGraphicsEngine from struct defaults to actual rendering logic
- Expand TestModSystem from empty-state checks to real mod loading
- Add stress/contention tests to TestWorkSema, TestLockFreeRingAllocator
- Add branching/choice tests to TestDialogueSystem
- Add rollback accuracy tests to TestClientPrediction

## Notes

- 74% of tests use standalone implementations (no engine headers) for CI/Linux compatibility
- Test framework: custom TestFramework.h with EXPECT/ASSERT macros
- All 211 test files compile and are in CMakeLists.txt
- Cross-platform: standalone reimplementations avoid DirectXMath dependency
- Naming convention: `Test<SystemName>.cpp`
