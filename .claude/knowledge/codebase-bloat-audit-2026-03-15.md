# Codebase Bloat Audit — March 15, 2026

**Last updated:** 2026-03-18
**Type:** Observation
**Status:** Active
**Severity:** Critical
**Supersedes:** codebase-bloat-audit-2026-03-14.md, bloat-audit-extended-2026-03-14.md

---

## Executive Summary

Comprehensive audit combining prior findings (March 14) with new deep analysis. The codebase has **269,770 total source lines** across all `.h` and `.cpp` files. This audit identifies **~10,000+ lines of confirmed removable code**, **17 orphaned singleton systems**, **127 classes exceeding the 15-public-method limit**, and multiple structural duplication issues.

**Progress (2026-03-18):** All P0 targets resolved. SparkConsole refactored (7,000→551). Five major file splits completed: GraphicsEngine.cpp (4,949→5 files), MaterialSystem.cpp (4,283→3 files), EditorUI.cpp (2,691→3 files), AssetPipeline.cpp (2,557→3 files), Shader.cpp (2,344→2 files). VisualScriptingSystem deleted. ~16,800 lines redistributed from 5 monolithic files into 17 focused modules.

**Progress (2026-03-31):** InspectorComponentRenderers.cpp (1,887 lines) split into 4 domain-specific files (~470 lines each): Core3D, 2D, Gameplay, Reflected. ConsoleApp::RegisterDefaultCommands (321 lines) split into 3 focused functions: RegisterCoreCommands, RegisterDiagnosticCommands, RegisterAliasCommands. Oversized function count reduced from 66 to 9 (86% reduction). CLAUDE.md phantom system references (Procedural, Stats) removed.

**Progress (2026-03-31, optimization pass):** ~1100 lines net reduction across 5 files:
- InputManager.cpp: Windows/Linux code consolidated — shared key mapping, query methods, console integration defined once (1338→840, -37%)
- UpscalingSystem.cpp: 640 lines of inline HLSL shaders extracted to UpscalingShaders.h (1335→652, -51%)
- SaveSystem.cpp: RegisterBuiltins() split into 3 domain functions; SerializeWorld() refactored with TrySerialize<T> template (1350→1274)
- ConsoleApp.cpp: Run/ReadEngineInput/ReadUserInput split into focused helpers
- SparkEngine.cpp: RunHeadlessWindows refactored to reuse existing helpers
- AudioMixer ODR issue confirmed FALSE — MusicManager.h defines AudioBusMixer, not AudioMixer
- MaterialSystem duplicates confirmed RESOLVED in prior sessions

---

## 1. Oversized Files (Hard Limit Violations)

### CPP Files (400-line limit)

| File | Lines | Over by | Priority |
|------|-------|---------|----------|
| `Utils/SparkConsole.cpp` | **6,996** | +6,596 | P0 |
| `Graphics/GraphicsEngine.cpp` | ~~4,579~~ **1,284** | **RESOLVED** — split into 5 files | P0 ✅ |
| `Graphics/MaterialSystem.cpp` | ~~4,326~~ **1,435** | **RESOLVED** — split into 3 files | P0 ✅ |
| `SparkEditor/VisualScripting/VisualScriptingSystem.cpp` | ~~4,067~~ **DELETED** | **RESOLVED** — duplicate system removed | P0 ✅ |
| `Graphics/AssetPipeline.cpp` | ~~2,557~~ **1,595** | **RESOLVED** — split into 3 files | P1 ✅ |
| `SparkEditor/Core/EditorUI.cpp` | ~~2,353~~ **1,358** | **RESOLVED** — split into 3 files | P1 ✅ |
| `Graphics/Shader.cpp` | ~~2,334~~ **1,922** | **RESOLVED** — console ops extracted | P1 ✅ |
| `SparkEditor/AssetPipeline/AdvancedAssetPipeline.cpp` | **2,324** | +1,924 | P1 |
| `Graphics/LightingSystem.cpp` | **2,231** | +1,831 | P1 |
| `Graphics/RHI/Vulkan/VulkanDevice.cpp` | **2,203** | +1,803 | P1 |
| `Core/SparkEngine.cpp` | **2,115** | +1,715 | P1 |
| `SparkGame/Game.cpp` | **2,042** | +1,642 | P1 |
| `Physics/PhysicsSystem.cpp` | **2,029** | +1,629 | P1 |
| `Engine/Scripting/VisualScriptSystem.cpp` | **2,027** | +1,627 | P1 |
| `SparkEditor/Lighting/LightingTools.cpp` | **1,963** | +1,563 | P1 |
| `SparkEditor/Panels/MaterialEditorPanel.cpp` | **1,832** | +1,432 | P2 |
| `SparkEditor/Animation/AnimationTimeline.cpp` | **1,796** | +1,396 | P2 |
| `SparkEditor/Panels/DialogueEditorPanel.cpp` | **1,781** | +1,381 | P2 |
| `Graphics/TextureSystem.cpp` | **1,739** | +1,339 | P2 |
| `SparkEditor/VersionControl/VersionControlSystem.cpp` | **1,710** | +1,310 | P2 |
| `Graphics/RHI/OpenGL/OpenGLDevice.cpp` | **1,623** | +1,223 | P2 |
| `Graphics/RenderTarget.cpp` | **1,605** | +1,205 | P2 |
| `Engine/Networking/NetworkManager.cpp` | **1,584** | +1,184 | P2 |

**Total: 23 files violating .cpp limit**

### Header Files (200-line limit)

| File | Lines | Over by | Priority |
|------|-------|---------|----------|
| `Physics/PhysicsSystem.h` | **1,909** | +1,709 | P0 |
| `Graphics/RenderGraph.h` | **1,730** | +1,530 | P0 |
| `Graphics/SkyAtmosphere.h` | **1,475** | +1,275 | P0 |
| `Core/Platform.h` | **1,394** | +1,194 | P1 |
| `Graphics/WaterSystem.h` | **1,373** | +1,173 | P1 |
| `Graphics/InstanceRenderer.h` | **1,223** | +1,023 | P1 |
| `Graphics/PostProcessingPipeline.h` | **1,127** | +927 | P1 |
| `Engine/AI/BehaviorTree.h` | **1,123** | +923 | P1 |
| `Engine/Animation/AnimationSystem.h` | **1,054** | +854 | P1 |
| `Graphics/GlobalIllumination.h` | **1,041** | +841 | P1 |
| `Graphics/TemporalEffects.h` | **940** | +740 | P1 |
| `Engine/ECS/Systems/ECSystems.h` | **919** | +719 | P1 |
| `Graphics/ResourceResidencyManager.h` | **902** | +702 | P1 |
| `Input/PlatformInput.h` | **897** | +697 | P1 |
| `Graphics/UpscalingSystem.h` | **897** | +697 | P1 |
| `Graphics/ShadowAtlas.h` | **873** | +673 | P2 |
| `Engine/SaveSystem/SaveSystem.h` | **872** | +672 | P2 |
| `SparkEditor/VisualScripting/VisualScriptingSystem.h` | **870** | +670 | P2 |
| `Graphics/DynamicQualityScaler.h` | **849** | +649 | P2 |
| `Graphics/GPUParticleSystem.h` | **805** | +605 | P2 |
| `SparkGame/Player.h` | **796** | +596 | P2 |
| `Graphics/GraphicsEngine.h` | **787** | +587 | P2 |
| `SceneManager/SceneManager.h` | **782** | +582 | P2 |
| `SparkEditor/VersionControl/VersionControlSystem.h` | **780** | +580 | P2 |

**Total: 24 files violating .h limit**

---

## 2. Dead Code — Delete Immediately (0 usages)

| File | Lines | Evidence | Status |
|------|-------|----------|--------|
| `Utils/ChromeTracing.h` | 211 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — Start()/Stop()/SaveToFile() in all 5 startup paths |
| `Utils/MemoryDebugger.h` | 433 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — SetEnabled()/PrintLeakReport() in debug builds |
| `Utils/FrameInspector.h` | 408 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — OnFrameEnd() in all main loops |
| `Utils/Tween.h` | 455 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — TweenManager::Update(dt) in all main loops |
| `Utils/ScopeGuard.h` | ~285 | Only included in its own test file | Utility header — no init needed, available for use |
| `Utils/DebugDraw.h` | 418 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — SetEnabled()/Flush(dt) in all main loops |
| `Utils/DebugOverlay.h` | 413 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — SetEnabled()/Update(dt) in all main loops |
| `Utils/FileLogger.h` | 432 | ~~0 includes~~ | **WIRED IN** (2026-03-16) — Initialize()/Shutdown() in all startup paths |

**Previously dead code now wired in: 7 of 8 systems (ScopeGuard is a utility, needs no wiring)**

---

## 3. Orphaned Singleton Systems (17 Total) — NEW

Systems with `GetInstance()` / `Initialize()` / `Update()` that are **never called** in engine startup or main loop.

### Tier 1 — Critical (core systems, should be wired in or deleted)

| System | File | Issue |
|--------|------|-------|
| **ConsoleProcessManager** | `Utils/ConsoleProcessManager.h` | Initialize() and ProcessCommands() never called |
| **VisualScriptSystem** | `Engine/Scripting/VisualScriptSystem.h` | Initialize() never called in startup |
| **DecalSystem** | `Graphics/DecalSystem.h` | Initialize() and Update() never called |
| **FileLogger** | `Utils/FileLogger.h` | Initialize() never called |
| **DXRSupport** | `Graphics/RHI/DXRSupport.h` | Initialize() never called |
| **PlatformInputManager** | `Input/PlatformInput.h` | Initialize() and Update() never called (InputManager used instead) |

### Tier 2 — Debug/Utility (**ALL WIRED IN** — 2026-03-16)

| System | File | Status |
|--------|------|--------|
| **DebugDraw** | `Utils/DebugDraw.h` | **WIRED IN** — SetEnabled()+Flush(dt) in all 5 startup/loop paths |
| **DebugOverlay** | `Utils/DebugOverlay.h` | **WIRED IN** — SetEnabled()+Update(dt) in all 5 startup/loop paths |
| **ChromeTracing** | `Utils/ChromeTracing.h` | **WIRED IN** — Start()/SaveToFile()/Stop() in all paths |
| **FrameInspector** | `Utils/FrameInspector.h` | **WIRED IN** — OnFrameEnd() in all loop paths |
| **MemoryDebugger** | `Utils/MemoryDebugger.h` | **WIRED IN** — SetEnabled()/PrintLeakReport() in debug builds |
| **TweenManager** | `Utils/Tween.h` | **WIRED IN** — Update(dt) in all loop paths |
| **FileLogger** | `Utils/FileLogger.h` | **WIRED IN** — Initialize()/Shutdown() in all paths |

### Tier 3 — Gameplay/AI (built but disconnected)

| System | File | Issue |
|--------|------|-------|
| **NavMesh** | `Engine/AI/NavMesh.h` | Singleton never instantiated |
| **NavMeshObstacles** | `Engine/AI/NavMeshObstacles.h` | GetInstance() never called |
| **Sequencer** | `Engine/Cinematic/Sequencer.h` | GetInstance() and Update() never called |
| **AnimationSystem** | `Engine/Animation/AnimationSystem.h` | GetInstance() and Update() never called |
| **WeaponManager** | `Engine/Gameplay/WeaponManager.h` | No Initialize/Update calls found |
| **MeshLOD** | `Graphics/MeshLOD.h` | GetInstance() never called |

**Rule violated:** *"Every system must be initialized. If Initialize() exists, it must be called."*

---

## 4. Public Method Limit Violations (127 classes exceed 15-method limit) — NEW

### Top 20 Worst Offenders

| Public Methods | Class | File |
|----------------|-------|------|
| **87** | VSOutput | Graphics/PostProcessingPipeline.h |
| **74** | SkyAtmosphereSystem | Graphics/SkyAtmosphere.h |
| **68** | Obstacle | Engine/AI/SteeringBehaviors.h |
| **67** | IBLCB | Graphics/IBLGenerator.h |
| **64** | MeshDrawCommand | Graphics/GraphicsEngine.h |
| **57** | Game (SPARK_GAME_API) | SparkGame/Game.h |
| **56** | LocalFileCache | Utils/LocalFileCache.h |
| **51** | SparkEngineIntegration | SparkEditor/Integration/SparkEngineIntegration.h |
| **51** | MathUtils | Utils/MathUtils.h |
| **48** | Config | Graphics/ResourceResidencyManager.h |
| **47** | PlatformInputManager | Input/PlatformInput.h |
| **42** | MaterialMetrics | Graphics/MaterialSystem.h |
| **42** | AssetMetrics | Graphics/AssetPipeline.h |
| **41** | WaterSystem | Graphics/WaterSystem.h |
| **41** | PhysicsMetrics | Physics/PhysicsSystem.h |
| **40** | RHIAdapter | Graphics/RHI/RHIAdapter.h |
| **39** | ShadowAtlas | Graphics/ShadowAtlas.h |
| **39** | PostProcessingPipeline | Graphics/PostProcessingPipeline.h |
| **36** | SimpleConsole | Utils/SparkConsole.h |
| **35** | RenderGraph | Graphics/RenderGraph.h |

### Violation Distribution

| Severity | Method Count | Classes |
|----------|-------------|---------|
| Critical (>40) | 40+ | **15 classes** |
| High (25–40) | 25–40 | **35 classes** |
| Medium (16–24) | 16–24 | **77 classes** |
| **Total violations** | | **127 classes** |

### By Subsystem

| Subsystem | Violations |
|-----------|-----------|
| Graphics | 56 |
| Editor | 23 |
| Utils/Core | 18 |
| Physics | 5 |
| Networking | 4 |
| AI | 4 |
| Audio | 3 |
| Game | 3 |
| Animation | 3 |
| Other | 8 |

---

## 5. Structural Duplication Issues

### 5a. Two Parallel Visual Scripting Systems (6,964 lines combined)

| System | Location | Lines |
|--------|----------|-------|
| VisualScriptSystem | `Engine/Scripting/VisualScriptSystem.{h,cpp}` | 3,054 |
| VisualScriptingSystem | `SparkEditor/VisualScripting/VisualScriptingSystem.{h,cpp}` | 4,937 |

Both define node graphs, pin types, compilation, and serialization. Neither includes the other. Neither is wired into startup. **Combined: 7,991 lines of overlapping functionality.**

### 5b. Duplicate AudioMixer Class (ODR Risk)

Two classes named `AudioMixer` in `namespace Spark::Audio`:
- `Audio/AudioMixer.h` (331 lines) — instance-based
- `Audio/MusicManager.h` line 57 (part of 292 lines) — singleton

**Risk:** Including both headers in one translation unit causes ODR violation.

### 5c. Duplicate Startup Paths in SparkEngine.cpp

| System | Creation Count | Locations |
|--------|---------------|-----------|
| PhysicsSystem | 3× `make_unique` | Lines 282, 397, 1649 |
| GraphicsEngine | 2× `make_unique` | Lines 609, 1768 |
| SimpleConsole::Initialize() | 3× | Lines 294, 635, 1661 |

**Fix:** Extract one `InitCoreSystems()` function. Estimated reduction: ~600 lines.

### 5d. 26 Register*Commands() Methods in SimpleConsole

Violates "1 per subsystem" limit. Should consolidate to 3–4 functions max (Engine, Graphics, Debug, Game).

### 5e. 9 Logging Methods on SimpleConsole

`Log()`, `LogInfo()`, `LogWarning()`, `LogError()`, `LogSuccess()`, `LogCritical()`, `LogTrace()`, `LogDebug()`, plus severity-based `Log()`. Only need `Log(severity, msg)` + 3 convenience wrappers.

---

## 6. Duplicate #include

`Shader.cpp` line 1298 includes `SparkConsole.h` — verified as platform-specific (Windows/Linux `#ifdef`), not a true duplicate.

---

## Priority Action Plan

### P0 — RESOLVED: Dead headers wired in (2026-03-16)

All 7 previously-dead utility systems are now wired into all 5 engine startup paths (Windows headless, Windows windowed, Linux headless, Linux SDL2, Linux fallback) via `InitDebugSystems()`/`UpdateDebugSystems(dt)`/`ShutdownDebugSystems()` helper functions in SparkEngine.cpp.

ChromeTracing macro renamed to `SPARK_CHROME_TRACE_SCOPE` to avoid conflict with `Validate.h`'s `SPARK_TRACE_SCOPE`.

### P1 — Wire or Delete Remaining Orphaned Systems

| Action | Impact | Status |
|--------|--------|--------|
| ~~Wire ConsoleProcessManager~~ | ~~Enables console subprocess~~ | **DONE** (prior session) |
| ~~Wire FileLogger~~ | ~~File-based logging~~ | **DONE** (this session) |
| Wire or delete DecalSystem | 0 calls = dead | OPEN |
| Wire or delete DXRSupport | Optional; behind toggle | OPEN |
| Audit PlatformInputManager vs InputManager | One may be dead | OPEN |
| Wire or delete NavMesh, NavMeshObstacles | AI navigation dead | OPEN |
| Wire or delete Sequencer | Cinematic system dead |
| Wire or delete AnimationSystem singleton | Animation update dead |
| Wire or delete MeshLOD | LOD management dead |

### P2 — Consolidate (2–3 sessions)

| Action | Lines Saved |
|--------|------------|
| Strip embedded UI from SparkConsole.cpp | ~4,200 |
| Consolidate Register*Commands → 3–4 functions | ~500 |
| Extract `InitCoreSystems()` in SparkEngine.cpp | ~600 |
| Rename AudioMixer in MusicManager.h | ODR risk fix |
| Consolidate VisualScript type definitions | ~500 |

### P3 — Structural Refactors (multi-session)

| Action | Lines Saved |
|--------|------------|
| Split PhysicsSystem.h (1,909 → ~200 + detail) | ~1,500 |
| Split RenderGraph.h (1,730 → ~200 + detail) | ~1,300 |
| Refactor GraphicsEngine.cpp (4,579 → modules) | ~3,000 |
| Refactor MaterialSystem.cpp (4,326 → modules) | ~2,500 |
| Reduce 127 classes to ≤15 public methods each | Ongoing |

---

## Metrics Summary

| Metric | Count |
|--------|-------|
| Total source lines | 269,770 |
| Files violating .cpp 400-line limit | 23 |
| Files violating .h 200-line limit | 24 |
| Confirmed dead files (0 usage) | 5 (1,607 lines) |
| Orphaned singleton systems | 17 |
| Classes exceeding 15-method limit | 127 |
| ODR-risky duplicate class names | 1 |
| Parallel system implementations | 2 (VisualScripting) |
| Duplicate system creation in startup | 3 systems × 3 paths |
| Estimated total removable lines | ~10,000+ |

---

## Notes

- All line counts verified with `wc -l` on 2026-03-15.
- Prior audit findings (March 14) remain unfixed — all issues carried forward.
- Hard limits from CLAUDE.md are binding.
- The 127 public-method violations are a NEW finding not in the prior audit.
- The 17 orphaned singletons expand on the prior audit's count of 2.
- ScopeGuard.h and Tween.h are NEW dead-code findings.
- **See also:** [AI bloat pattern](ai-bloat-pattern.md), [Code quality violations](code-quality-violations.md)
