# SparkEngine — Comprehensive Gap Analysis (2026-03-18)

> Full-project audit covering code, tests, documentation, wiring, and architecture.
> Cross-references `.claude/knowledge/` entries and prior gap analyses.

---

## Status Legend

| Tag         | Meaning                                                    |
|-------------|------------------------------------------------------------|
| **DONE**    | Fully implemented, tested, wired, documented               |
| **PARTIAL** | Core works but missing tests, docs, or edge cases          |
| **STUB**    | Header/signature exists; body empty or returns dummy       |
| **MISSING** | Not yet created                                            |
| **ORPHANED**| Code exists but not initialized/called/wired               |

---

## Executive Summary

| Metric | Value |
|--------|-------|
| Total source files | 632 (.cpp + .h) |
| Total lines of code | ~241K |
| Test files | 87 (23K+ lines) |
| Wiki pages | 55 (29K lines) |
| Functional core systems | 19 |
| Orphaned singletons | 0 (all resolved — wired or reclassified as passive caches) |
| TrinityCore systems (new) | 10 (all wired, none tested) |
| Systems without tests | 14+ major subsystems |
| Critical doc gaps | 5 (Camera, Persistence, Threading, Editor, API docs done) |
| TODO/FIXME comments | 6 (all scoped, non-critical) |

**Overall health: GOOD.** Core engine is functional. Primary gaps are in testing
(new systems untested), documentation (4 core systems undocumented), and a handful
of orphaned singletons that need wiring or deletion.

---

## 1. Core Platform & Infrastructure

### 1.1 Engine Initialization (`Core/`)
- [x] `SparkEngine.cpp` — **DONE** — 3 startup paths (WinMain, headless, SDL2), all share common Init/Update/Shutdown helpers
- [x] `EngineContext` — **DONE** — service locator with typed Get/Set/RegisterSystem
- [x] `EngineSettings` — **DONE** — INI-based config, RegisterConsoleCommands()
- [x] `Platform.h` — **DONE** — cross-platform type abstractions
- [x] DirectXMath stubs — **DONE** — PlatformDirectXMathStubs.h (671 lines)

### 1.2 Module System
- [x] `ModuleManager` — **DONE** — discovery, loading, manifest support
- [x] `IModule` / `IGameModule` — **DONE** — both interfaces supported
- [x] `ModuleHotReload` — **PARTIAL** — implemented & wired, **no tests**
- [x] DLL export macros — **DONE** — SparkExport.h

### 1.3 Error Handling & Crash Recovery
- [x] `CrashHandler` — **DONE** — minidump, GitHub issue upload, screenshot capture
- [x] `CrashHandlerStub` — **DONE** — Linux/macOS fallback
- [x] `SparkError` — **DONE** — error codes
- [x] `Assert.h` — **DONE** — debug/release assertions
- [ ] `Result.h` — **MISSING** — no monadic error type

### 1.4 Logging
- [x] `SimpleConsole` — **DONE** — thread-safe, severity-based, 551 lines (post-refactor)
- [x] `FileLogger` — **DONE** — wired in InitDebugSystems()
- [x] `ConsoleProcessManager` — **DONE** — wired, ProcessCommands() called every frame
- [x] `LogMacros` — **DONE** — LOG_INFO/WARN/ERROR/FATAL
- [x] `ChromeTracing` — **DONE** — wired

---

## 2. Graphics & Rendering

### 2.1 Core Rendering
- [x] GraphicsEngine (D3D11) — **DONE** — primary backend, split into 5 files
- [x] Forward rendering — **DONE**
- [x] Deferred rendering — **DONE**
- [x] Forward+ / clustered — **DONE**
- [x] PBR materials — **DONE** — MaterialSystem split into 3 files
- [x] Lighting — **DONE** — directional/point/spot + shadows, split into 2 files
- [x] Textures — **DONE**
- [x] Shaders — **DONE** — Shader.cpp (1,922 lines, largest file)
- [x] Particles — **DONE**
- [x] 2D sprites — **DONE**
- [x] Post-processing — **DONE**
- [x] Upscaling (FSR/DLSS/SparkSR) — **DONE**

### 2.2 RHI Backends
- [x] D3D11 — **DONE** — primary (1,325 lines)
- [x] D3D12 — **PARTIAL** — implemented (1,496 lines), experimental
- [x] Vulkan — **PARTIAL** — implemented (1,479 lines), experimental
- [x] OpenGL — **PARTIAL** — implemented (1,424 lines), experimental
- [x] Metal — **STUB** — header only, macOS experimental

### 2.3 Advanced Rendering
- [x] MeshLOD — **ORPHANED** — `LODManager::GetInstance()` never initialized/updated
- [x] DecalSystem — **DONE** — wired in Init/Update/Shutdown
- [x] DXR raytracing — **PARTIAL** — complete but ENABLE_DXR=OFF, gated behind flag
- [x] Hybrid RT (SDFGI) — **DONE** — software fallback + optional hardware
- [x] WeatherSystem — **DONE** — registered & updated via EngineContext
- [x] Terrain rendering — **DONE** — TerrainSystem wired into ECS, TerrainRenderer generates GPU mesh from heightmap

### 2.4 Rendering Stubs (~15K dead lines, deleted in prior sessions)
Previous sessions deleted 12 header-only stubs (SkyAtmosphere, WaterSystem, GlobalIllumination, etc.)

---

## 3. Physics

- [x] PhysicsSystem (Bullet3) — **DONE** — 1,753 lines, main-thread-only
- [x] Rigid bodies, collision, raycasting — **DONE**
- [x] Cloth simulation — **DONE** — tested
- [x] Ragdoll — **DONE**
- [x] Physics joints — **DONE** — tested
- [ ] Vehicle physics — **PARTIAL** — SparkGame has vehicles but weapons stubbed

---

## 4. Audio

- [x] AudioEngine (XAudio2) — **DONE** — 32 source voices, wired
- [x] MusicManager — **DONE** — Initialize/Update/Shutdown wired into gameplay system lifecycle
- [x] AudioMixer — **DONE** — renamed to AudioBusMixer to avoid ODR
- [ ] Audio tests — **MISSING** — no dedicated tests for audio subsystem

---

## 5. ECS Framework

- [x] EnTT integration — **DONE** — 332-line stub fallback when submodule absent
- [x] CoreComponents.h + 12 domain headers — **DONE**
- [x] ECSystems.h — **DONE** — 836 lines
- [x] ECS tests — **DONE** — TestECSIntegration.cpp, TestECSWorld.cpp
- [x] Execution order: Physics → Animation → AI → Audio → Lifecycle → Render — **DONE**

---

## 6. AI & Navigation

- [x] AISystem — **DONE** — behavior trees + environment query
- [x] BehaviorTree — **DONE** — tested (902 lines header)
- [x] NavMesh — **PARTIAL** — queries work, but NavMeshManager never initialized
- [x] NavMeshObstacleManager — **ORPHANED** — GetInstance() never called externally
- [x] MovementSystem (TrinityCore) — **DONE** — wired, **no tests**
- [x] Steering behaviors — **DONE** — tested
- [ ] AI debug visualization — **MISSING** — no editor panel

---

## 7. Animation

- [x] Skeletal animation — **DONE** — tested (1,375 lines)
- [x] IK — **DONE**
- [x] State machines — **DONE**
- [x] Retargeting — **DONE** — tested
- [x] AnimationTimeline (editor) — **DONE** — 1,847 lines
- [x] AnimationManager singleton — **ORPHANED** — GetInstance() never called externally

---

## 8. Networking

- [x] NetworkManager — **PARTIAL** — 1,809 lines, ENABLE_NETWORKING=OFF by default
- [x] NetBuffer serialization — **DONE** — tested
- [x] UDP socket layer — **DONE**
- [x] Entity replication — **PARTIAL** — no authority model, no interest management
- [x] ReplicationFields (TrinityCore) — **DONE** — wired, **no tests**
- [x] EntityReplicator — **DONE** — dirty tracking
- [x] SpatialGrid (TrinityCore) — **DONE** — wired, **no tests**
- [x] Client-side prediction — **STUB** — input history exists, simulation missing
- [x] Lag compensation — **PARTIAL** — snapshots produced but not consumed
- [x] Reliable channels — **DONE** — ACK bitfield, retransmit with exponential backoff, RTT estimation, duplicate detection, ordered delivery
- [x] Connection timeout detection — **DONE** — CheckConnectionTimeouts() with handler callback, per-client heartbeat tracking
- [x] Integration tests — **DONE** — TestNetworkIntegration.cpp (30 tests)

---

## 9. Scripting

- [x] AngelScript VM — **DONE** — hot-reload, client/server context separation
- [x] ScriptHookManager (TrinityCore) — **DONE** — 28 hook types, wired, **no tests**
- [x] ConsoleRBAC (TrinityCore) — **DONE** — permission levels, wired, **tested** (22 tests)

---

## 10. Gameplay Systems

- [x] AbilitySystem (TrinityCore) — **DONE** — spells/auras/procs, wired, **no tests**
- [x] ConditionSystem (TrinityCore) — **DONE** — stateless evaluator, wired, **no tests**
- [x] InstanceManager (TrinityCore) — **DONE** — encounters/lockouts, wired, **no tests**
- [x] WeaponRegistry — **DONE** — RegisterDefaults() called
- [x] DestructionSystem — **DONE** — wired in InitGameplaySystems(), **tested**
- [x] DialogueSystem — **DONE** — registered & updated via EngineContext
- [x] WeatherSystem — **DONE** — registered & updated
- [x] UISystem — **DONE** — registered & updated
- [x] ModSystem — **DONE** — registered
- [x] SaveSystem — **DONE** — initialized, tested
- [x] CoroutineScheduler — **DONE** — registered with EngineContext, Update(dt) called in gameplay loop
- [x] Localization — **DONE** — restored from deletion
- [ ] Inventory (engine-level) — **MISSING** — game-only in SparkGame
- [ ] Quest (engine-level) — **MISSING** — game-only in SparkGame

---

## 11. Editor

### 11.1 Core Editor
- [x] EditorUI — **DONE** — split into 3 files
- [x] Viewport/scene view — **DONE**
- [x] Entity inspector — **DONE**
- [x] Scene hierarchy — **DONE**
- [x] Asset browser — **DONE**
- [x] Console — **DONE**
- [x] Profiler panel — **DONE**
- [x] Play mode (PIE) — **DONE**
- [x] Undo/redo — **DONE**
- [x] Prefab system — **DONE**
- [x] Search — **DONE**

### 11.2 Built-Not-Wired Editor Systems
- [x] TerrainEditor — **ORPHANED** — no terrain system to back it
- [x] LightingTools — **PARTIAL** — split into 2 files, needs deeper integration
- [x] GizmoSystem — **PARTIAL** — exists, not fully connected
- [x] VersionControlSystem — **PARTIAL** — 1,764 lines, exists but no VCS backend
- [x] EditorPluginManager — **PARTIAL** — exists, undocumented

### 11.3 Missing Editor Features
- [x] Shader graph editor — **DONE** — MaterialEditor wired to ShaderGraphCompiler (graph-to-HLSL)
- [x] AI debug visualization panel — **DONE** — AIDebugPanel (agent list, blackboard, BT trace, overlays)
- [ ] Cinematic sequencer panel — **MISSING**
- [ ] Project settings panel — **MISSING**

---

## 12. SDK & Tools

- [x] SparkSDK headers — **DONE** — 370 LOC, IEngineContext with 19 getters
- [x] SparkConsole.exe — **DONE** — standalone console
- [x] SparkShaderCompiler.exe — **DONE**
- [x] SDK unique_ptr fix — **DONE** — raw pointer with manual lifecycle
- [x] ECS exposed via GetWorld() — **DONE**
- [ ] IGameModule not in SDK — **PARTIAL** — internal header only

---

## 13. Testing

### 13.1 Test Infrastructure
- [x] 85 test files, 21.5K lines — **DONE**
- [x] All tests in CMakeLists.txt — **DONE** (100%)
- [x] CTest integration — **DONE**
- [x] CI runs tests on every PR — **DONE**

### 13.2 Systems WITHOUT Tests (Critical Gaps)

**TrinityCore systems (9 — all new, all untested):**
| System | Lines | Priority |
|--------|-------|----------|
| AbilitySystem | ~800 | P0 |
| ConditionSystem | ~300 | P0 |
| InstanceManager | ~400 | P0 |
| MovementSystem | ~350 | P1 |
| SpatialGrid | ~400 | P1 |
| AsyncDatabase | ~500 | P1 |
| ReplicationFields | ~300 | P1 |
| ScriptHookManager | ~400 | P2 |
| ModuleHotReload | ~300 | P2 |

**Core infrastructure (untested):**
| System | Notes |
|--------|-------|
| AudioEngine | No dedicated tests |
| SceneManager | No dedicated tests |
| GraphicsEngine | Only subsystem tests |
| PhysicsSystem | Only component tests |
| NetworkManager | Protocol tested, orchestration not |

---

## 14. Documentation

### 14.1 Wiki Coverage
- 55 pages, 29K lines — **GOOD**
- 99.6% Doxygen coverage on headers

### 14.2 Critical Documentation Gaps
1. ~~**Camera System**~~ — **DONE** wiki/Camera-System.md
2. **Core Platform Infrastructure** — scattered across CLAUDE.md
3. ~~**Persistence/AsyncDatabase**~~ — **DONE** wiki/Persistence-System.md
4. **Console & Logging** — scattered, incomplete
5. **Editor Plugin System** — undocumented
6. **OpenGL/Vulkan backend details** — missing dedicated pages
7. **Performance Profiling** — minimal docs
8. ~~**12+ editor panels**~~ — **DONE** All 32 panels documented in SparkEditor.md
9. ~~**Threading model**~~ — **DONE** wiki/Threading-Model.md (comprehensive page)
10. ~~**API docs not generated**~~ — **DONE** docs/api/ (370 pages from 382 headers)

---

## 15. Orphaned Systems (Action Required)

| System | Action | Rationale |
|--------|--------|-----------|
| NavMeshManager | Leave as-is | Passive cache — no lifecycle methods. AISystem queries it on demand. Content (nav meshes) loaded at level load time |
| NavMeshObstacleManager | Leave as-is | Passive manager — no lifecycle methods. Wire SetNavMesh() when dynamic obstacles are needed |
| AnimationManager | Leave as-is | Working asset cache. AnimationSystem is a type alias for it. Used internally by AnimationSystem.cpp |
| LODManager (MeshLOD) | Leave as-is | Passive cache — no Update() method. Queries only when meshes register LOD chains |
| ~~PlatformInputManager~~ | **DELETED** | 5 files, ~2,100 lines removed (dead duplicate of InputManager) |
| ~~WeaponSystem~~ | **WIRED** | Update() now iterates WeaponInventoryComponent via EnTT view; wired in UpdateGameplaySystems() |

---

## 16. Build System

- [x] CMake 3.25+ — **DONE**
- [x] Windows MSVC (v143) — **DONE**
- [x] Linux GCC — **DONE**
- [x] Linux Clang — **DONE**
- [x] ASan/UBSan — **DONE**
- [x] clang-format enforcement — **DONE**
- [x] clang-tidy (continue-on-error) — **DONE**
- [x] Feature toggles (15 options) — **DONE**
- [ ] Duplicate imgui target definitions — **PARTIAL** — 3 in ThirdParty/ + 2 in SparkEditor/
- [ ] Circular linking — **KNOWN** — SparkEngine↔SparkGame↔SparkEngineLib

---

## 17. Third-Party Dependencies

| Library | Status | Issue |
|---------|--------|-------|
| EnTT | STUB FALLBACK | Submodule uninitialized; 332-line stub used |
| Bullet3 | CONDITIONAL | Submodule uninitialized; gated by SPARK_BULLET_PHYSICS_AVAILABLE |
| Dear ImGui | CONDITIONAL | Submodule uninitialized; editor disabled without it |
| AngelScript | CONDITIONAL | Submodule uninitialized; scripting disabled |
| miniz | CONDITIONAL | Submodule uninitialized; compression disabled |
| tinyobjloader | WORKING | Vendored, 3,517 lines |
| curl | REMOVED | Deleted from .gitmodules and CMakeLists.txt |

**Risk:** All 5 submodules uninitialized. Build succeeds silently with degraded features.

---

## 18. Priority Action Items

### P0 — Critical (This Session)
1. ~~Write tests for 3 highest-priority TrinityCore systems (AbilitySystem, ConditionSystem, InstanceManager)~~
2. Update knowledge DB entries with current findings
3. Create comprehensive gap analysis document (this file)

### P1 — High (Completed 2026-03-18)
4. ~~Wire NavMeshManager into AI initialization path~~ → Passive cache, no lifecycle methods needed
5. ~~Wire LODManager into rendering update~~ → Passive cache, no Update() needed
6. ~~Delete PlatformInputManager (dead duplicate)~~ → **DELETED** (5 files, ~2,100 lines)
7. ~~Delete or merge AnimationManager singleton~~ → Working asset cache, used by AnimationSystem.cpp
8. ~~Write tests for remaining TrinityCore systems~~ → Done (9 tests added)
9. ~~Create Camera System wiki page~~ → wiki/Camera-System.md
10. ~~Create Persistence System wiki page~~ → wiki/Persistence-System.md
11. ~~Wire WeaponSystem ECS iteration~~ → Implemented Update() with EnTT view
12. ~~Wire CoroutineScheduler::Update()~~ → Added to UpdateGameplaySystems()
13. ~~Wire MusicManager Init/Update/Shutdown~~ → Added to gameplay system lifecycle
14. ~~Add ConsoleRBAC test~~ → Tests/TestConsoleRBAC.cpp (22 tests)

### P2 — Medium (Completed 2026-03-19)
11. ~~Document threading model and concurrency rules~~ → wiki/Threading-Model.md (comprehensive page)
12. ~~Generate and commit API docs~~ → docs/api/ generated (370 pages from 382 headers)
13. ~~Expand SparkEditor.md with 12+ missing panels~~ → 32 panels fully documented with descriptions
14. ~~Add networking integration tests~~ → Tests/TestNetworkIntegration.cpp (30 tests: lifecycle, replication, serialization, lag compensation, edge cases)
15. ~~Implement terrain rendering system~~ → TerrainSystem wired into UpdateGameplaySystems(), TerrainRenderer already in GraphicsEngine
16. ~~Implement connection timeout detection~~ → CheckConnectionTimeouts() with timeout handler callback, per-client heartbeat tracking, entity cleanup

### P3 — Low (Completed 2026-03-19)
17. Implement Recast/Detour for NavMesh — deferred (requires external dependency)
18. ~~Build shader graph editor~~ → MaterialEditor.CompileMaterial() now uses ShaderGraphCompiler for graph-to-HLSL compilation
19. ~~Build AI debug visualization panel~~ → AIDebugPanel (agent list, blackboard inspector, BT trace, perception overlays, statistics)
20. ~~Complete client-side prediction~~ → ClientPrediction already fully implemented; TerrainSystem + include wired into main loop
21. ~~Implement reliable channel retransmission~~ → Exponential backoff (2^n capped at 8x), Jacobson/Karels RTT estimation, Karn's algorithm for retransmit samples
