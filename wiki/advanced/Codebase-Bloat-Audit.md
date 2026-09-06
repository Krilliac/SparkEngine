# Codebase Bloat Audit

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (audit/reference)
>
> **Platform/Backend Scope:** All platforms / all backends

## Overview

A point-in-time audit of oversized files, dead code, orphaned systems, and structural duplication, measured against the anti-bloat thresholds in `CLAUDE.md` (~500 lines for `.cpp`, ~300 lines for `.h`, ~15 public methods per class). The original audit (March 2026) was run against an earlier, much larger set of monolithic files; most of its P0/P1 offenders have since been split or deleted. This page carries forward the methodology and reports a **fresh largest-files scan as of 2026-06-08**.

Thresholds are guidelines for when to pause and think, not hard limits — a clean 450-line `.cpp` is fine; a cryptic 200-line one is not. See the Readability Principle in `CLAUDE.md`.

---

## 1. Current Largest Files (re-scanned 2026-06-08)

### Largest `.cpp` files (top offenders over the ~500-line guideline)

| File | Lines |
|------|-------|
| `Graphics/RHI/OpenGL/OpenGLDevice.cpp` | 2,103 |
| `Graphics/RHI/Vulkan/VulkanDevice.cpp` | 1,991 |
| `SparkEditor/Source/Panels/VisualScriptPanel.cpp` | 1,773 |
| `SparkEditor/Source/Profiler/PerformanceProfiler.cpp` | 1,609 |
| `Graphics/RHI/D3D12/D3D12Device.cpp` | 1,593 |
| `SparkEditor/Source/Core/EditorTheme.cpp` | 1,587 |
| `Graphics/PostProcessingPipeline.cpp` | 1,538 |
| `Core/EngineSettings.cpp` | 1,522 |
| `SparkEditor/Source/Core/EditorUI.cpp` | 1,516 |
| `SparkEditor/Source/Panels/ProjectSettingsPanel.cpp` | 1,501 |
| `Graphics/RHI/D3D11/D3D11Device.cpp` | 1,481 |
| `Graphics/GraphicsEngineWindows.cpp` | 1,481 |
| `SparkConsole/src/ConsoleApp.cpp` | 1,407 |
| `SparkEditor/Source/Communication/CollaborativeEditSession.cpp` | 1,373 |
| `SparkEditor/Source/Panels/InspectorComponentRenderers_Reflected.cpp` | 1,362 |
| `Core/Lifecycle/GameplayLifecycleShared.cpp` | 1,347 |
| `Utils/CrashHandler.cpp` | 1,322 |
| `Core/SparkEngineLinux.cpp` | 1,296 |
| `SparkEditor/Source/Panels/InspectorPanel.cpp` | 1,288 |
| `Engine/SaveSystem/SaveSystem.cpp` | 1,279 |

**`.cpp` files over 500 lines: 158.** The largest of the RHI backend files (OpenGL/Vulkan/D3D11/D3D12 devices) are single coherent units — full graphics-API surface implementations — and are reasonable candidates to leave intact rather than fragment.

### Largest `.h` files (top offenders over the ~300-line guideline)

| File | Lines |
|------|-------|
| `Graphics/RenderGraph.h` | 1,125 |
| `SparkEditor/Source/SceneSystem/SceneFileTypes.h` | 1,115 |
| `Core/EngineSettings.h` | 1,079 |
| `Utils/JsonUtils.h` | 963 |
| `Graphics/GraphicsEngine.h` | 957 |
| `Graphics/BasisTranscoder.h` | 912 |
| `Engine/ECS/Systems/ECSystems.h` | 849 |
| `Graphics/MeshClusterSystem.h` | 824 |
| `Graphics/SVGRenderer.h` | 803 |
| `Engine/DataTable/DataTableSystem.h` | 800 |
| `GameModules/SparkGameFPS/Source/Game/Player.h` | 795 |
| `Physics/PhysicsTypes.h` | 779 |
| `Engine/Networking/NetworkManager.h` | 732 |

**`.h` files over 300 lines: 216** (of which **64 exceed 500 lines**). Many of the largest are data-heavy headers (`SceneFileTypes.h`, `PhysicsTypes.h`, `JsonUtils.h`, `FastNoiseLite.h`, `FastNoise2SIMD.h`) where size is acceptable per the data-heavy-headers exception.

---

## 2. Status of the Original Audit's P0/P1 Offenders

Every one of the original audit's worst monolithic files has been resolved by splitting or deletion:

| Original offender | Then | Now | Status |
|-------------------|------|-----|--------|
| `Utils/SparkConsole.cpp` | 6,996 | 641 | Resolved — embedded UI stripped |
| `Graphics/GraphicsEngine.cpp` | 4,949 | 13 | Resolved — split into multiple files; `.cpp` is now a near-empty shim |
| `Graphics/MaterialSystem.cpp` | 4,326 | 486 | Resolved — split |
| Former Visual Scripting system implementation | 4,067 | deleted | Resolved — duplicate system removed |
| `Graphics/AssetPipeline.cpp` | 2,557 | split | Resolved |
| `SparkEditor/Source/Core/EditorUI.cpp` | 2,353 | 1,516 | Reduced (still over guideline) |
| `Graphics/Shader.cpp` | 2,334 | split | Resolved |

The original `Physics/PhysicsSystem.h` (1,909) and `Graphics/RenderGraph.h` (1,730) headers have shrunk to 685 and 1,125 respectively; `PhysicsTypes.h` (779) now holds the data definitions.

---

## 3. Structural Duplication — Resolved

### Two parallel visual-scripting systems → resolved

The original audit's biggest structural finding was two overlapping ~4,000-line visual-scripting systems (`Engine/Scripting/VisualScriptSystem` and `SparkEditor/VisualScripting/VisualScriptingSystem`) totaling ~8,000 lines, neither wired in. Today there is a clean split with distinct roles:

- `SparkEngine/Source/Engine/Scripting/VisualScriptCompiler.{h,cpp}` — the engine-side runtime/compiler.
- `SparkEditor/Source/Panels/VisualScriptPanel.{h,cpp}` — the editor authoring UI (1,773 lines, a top `.cpp` offender but a single coherent panel).

The duplicate editor `VisualScriptingSystem` is gone.

### AudioMixer ODR risk → resolved / never real

`AudioMixer` is now its own pair (`Audio/AudioMixer.{h,cpp}`); `MusicManager.h` defines a separately-named bus mixer, so there is no duplicate `AudioMixer` class. The original audit had already noted the ODR claim was FALSE.

---

## 4. Dead Code and Orphaned Systems

The original audit's "dead headers" (ChromeTracing, MemoryDebugger, FrameInspector, Tween, DebugDraw, DebugOverlay) were wired into the startup/loop paths in March 2026 and are no longer dead. **`FileLogger` is the exception:** it still has zero writers -- its two lifecycle calls were removed in the 2026-09 sweep. Engine log files come from `Spark::FileSink`, installed by `Logger::InstallDefaultSinks` (per-user `Logs/SparkEngine_<timestamp>.log`), and the `SPARK_FILE_LOG*` macros have no production caller. Where `FileLogger.h` is still present it is covered only by `Tests/TestDebugTools.cpp` and is pending a wire-it-in-or-delete decision.

For systems the original audit flagged as orphaned (DecalSystem, NavMesh, Sequencer, MeshLOD, etc.), a current reference scan shows each is now referenced from multiple engine `.cpp` files (DecalSystem: 6, NavMesh: 7, Sequencer: 3, MeshLOD: 3), indicating they have been wired in or are at least integrated into other systems. A definitive wired-in audit should use `tools/check-wiring.sh`, which is the maintained source of truth for "Initialize() exists ⇒ called."

---

## 5. How to Reproduce This Scan

```bash
# Largest .cpp / .h
find SparkEngine/Source SparkEditor/Source SparkConsole/src \
     SparkShaderCompiler/src GameModules -name '*.cpp' \
  | xargs wc -l | sort -rn | head -30

# Counts over threshold
find ... -name '*.cpp' | xargs wc -l | awk '$1>500 && $2!="total"' | wc -l
find ... -name '*.h'   | xargs wc -l | awk '$1>300 && $2!="total"' | wc -l

# Maintained checker
tools/check-bloat.sh
```

---

## Source & Freshness

- **Original audit:** `.claude/knowledge/codebase-bloat-audit-2026-03-15.md`, dated March 15, 2026 (last updated 2026-03-18, with progress notes through 2026-03-31).
- **Re-measured against codebase 2026-06-08.**
- OLD → NEW numbers updated:
  - Total source lines OLD 269,770 → NEW ~435,185 (`.h`+`.cpp`+`.hpp`; codebase grew substantially).
  - `.cpp` files over guideline OLD 23 (against a 400-line limit) → NEW 158 (against the current ~500-line guideline).
  - `.h` files over guideline OLD 24 (against a 200-line limit) → NEW 216 over 300 / 64 over 500.
  - Largest `.cpp` OLD `SparkConsole.cpp` 6,996 → NEW `OpenGLDevice.cpp` 2,103.
  - Largest `.h` OLD `PhysicsSystem.h` 1,909 → NEW `RenderGraph.h` 1,125.
- Findings now resolved since the original audit:
  - All P0 monolith splits confirmed: SparkConsole.cpp (6,996→641), GraphicsEngine.cpp (4,949→13), MaterialSystem.cpp (4,326→486), Shader.cpp, AssetPipeline.cpp, EditorUI.cpp.
  - Parallel VisualScripting duplication resolved (editor `VisualScriptingSystem` deleted; clean compiler/panel split remains).
  - AudioMixer ODR risk confirmed non-existent.
  - The 7 previously-dead utility headers are wired in; previously-orphaned gameplay systems now show multiple engine references.

## Related Pages

- [Codebase Statistics](Codebase-Statistics.md)
- [Codebase Health](Codebase-Health.md)
- [Stub and Abandoned Features](Stub-and-Abandoned-Features.md)
- [Contributing](Contributing.md)
