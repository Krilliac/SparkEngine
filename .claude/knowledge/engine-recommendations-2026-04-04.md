# Engine Recommendations — Next Steps (April 2026)

**Last updated:** 2026-04-09
**Type:** Decision
**Status:** Resolved

## Description

Fresh analysis of SparkEngine's gaps after all previous roadmap phases (1-5) are complete.
Based on three parallel deep-dives: infrastructure/maturity gaps, game module analysis, and test suite quality audit.

## Context

SparkEngine has ~519K LOC, 345 test files (4434 tests), 59 editor panels, 10 game modules,
6 RHI backends (only D3D11 renders), enterprise-grade networking (unused by game modules),
and extensive AI/physics/animation. Previous 5-phase roadmap and 20+ production systems
are all complete. This analysis identifies what remains.

## Snapshot Source/Date

> Snapshot source: `.claude/index.md`
> Snapshot date: `2026-04-09`
> Core metrics snapshot: `341 test files / 4416 tests / ~515K LOC / 1581 source files`

<!-- SNAPSHOT_SOURCE: .claude/index.md -->
<!-- SNAPSHOT_DATE: 2026-04-10 -->
<!-- CORE_METRICS: tests_files=345 tests_total=4434 loc_k=519 source_files=1602 -->

---

## Tier 1: Critical Gaps (Highest Impact)

### 1. Cross-System Integration Tests

**Problem:** 264 test files but nearly zero tests verifying systems working *together*.
No test creates a physics body on an ECS entity and checks the transform updates.
No test replicates an entity over the network. No test boots the engine with a real
RHI backend and renders a frame.

**What exists:** 6 named integration tests, but they use mocked sockets and NullRHI.
`TestFullEngineDiagnostics.cpp` is closest (boots 20+ subsystems, runs 200 frames)
but uses NullRHI — no GPU validation.

**Recommended tests:**
- Physics + ECS: Create rigid body, advance simulation, verify Transform component updated
- Rendering + ECS: Change entity transform, verify GPU world matrix buffer updated
- Networking + ECS: Replicate entity, simulate latency, verify client prediction
- Animation + Physics: Ragdoll activation, verify bone transforms match physics state
- Scene roundtrip: Save scene → load scene → verify identical entity/component state
- Audio + Streaming: Verify no audio stutter during background asset loads

**Impact:** Without these, integration bugs silently ship. This is the #1 testing gap.

### 2. Vulkan Backend to Feature Parity with D3D11

**Problem:** Only D3D11 renders. OpenGL has 1,932 lines of real code. Vulkan (1,722 lines),
D3D12 (1,558 lines), Metal are stubs. The RHI abstraction exists but is unvalidated —
if Vulkan can't render the same scene as D3D11, the abstraction is theoretical.

**Impact:** Blocks Linux/Steam Deck/macOS. Validates the entire RHI architecture.

### 3. Game Module Networking (Wire a Real Multiplayer Game)

**Problem:** Engine has 7,300 lines of enterprise-grade UDP networking (replication,
prediction, lag compensation, AES-256). Zero game modules use it. SparkGameMMO has
18 subsystems (guilds, trading, dungeons) but is entirely local-only.

**Recommendation:** Wire SparkGameFPS (21.7K LOC, most complete module) with real
networked multiplayer: player replication, projectile sync, hit validation.
Alternatively wire SparkGameMMO with the existing AreaServer/WorldServer architecture.

**Impact:** Proves the networking stack works end-to-end. Currently the most sophisticated
engine system with zero consumers.

### 4. Real Shader Files for GPU Rendering Systems

**Problem:** GPU binding layer exists for sky, water, terrain, GI, shadow atlas, SSAO,
volumetric fog, etc. — but no actual HLSL/GLSL shader files. The rendering systems
do CPU-side setup but produce no GPU output beyond D3D11's basic forward/deferred passes.

**Impact:** Advanced visual features are architecturally complete but visually inert.

---

## Tier 2: Important for Developer Experience

### 5. FBX Import Pipeline

**Problem:** `AssetPipeline.h` declares `LoadFBX()` but it's a stub. Most game artists
export FBX. Only OBJ and glTF are functional.

**Recommendation:** Integrate Assimp (already handles FBX/OBJ/glTF/Collada) or OpenFBX
(lightweight, single-file). Wire into AssetPipeline's existing async loading infrastructure.

### 6. Texture Compression Pipeline

**Problem:** No BC7/ASTC/Basis Universal compression. Textures go to GPU uncompressed,
wasting 4-8x VRAM. No build-time asset cooking step.

**Recommendation:** Add Basis Universal transcoder for runtime, compressonator or
ISPCTextureCompressor for build-time BC7/ASTC generation.

### 7. GPU Profiling & Frame Analysis

**Problem:** GPUPerfCounters.h and GPUDebugMarkers.h exist (PIX/RenderDoc markers),
CPU profiler works, but no GPU timestamp queries, no pipeline statistics queries,
no flame graph export.

**Recommendation:** Add D3D11 timestamp/pipeline-stats queries, expose in editor
PerformanceProfiler panel. Add RenderDoc frame capture trigger button in editor.

### 8. Shader Hot-Reload: Actual Compilation

**Problem:** `ShaderHotReload.h` (394 lines) has file watching and callback infrastructure,
but `CompileShader()` only checks file existence — it doesn't call D3DCompile or
glslang. Hot-reload is plumbed but doesn't work.

**Recommendation:** Wire `CompileShader()` to the existing D3DCompile path in
ShaderCompiler. Add error reporting to editor console on compilation failure.

---

## Tier 3: Polish & Maturity

### 9. CPack Packaging / SDK Distribution

**Problem:** CMake has install targets (~150 lines) and SparkEngineConfig.cmake,
but no CPack configuration. Can't generate installers, archives, or SDK packages.

### 10. Game Module Code Deduplication

**Problem:** SparkGameRPG reimplements QuestSystem and DialogueSystem separately
from the engine's versions. SparkGameARPG has its own loot/skill systems that
partially overlap engine gameplay systems.

**Recommendation:** Make engine QuestSystem/DialogueSystem extensible enough for
genre-specific needs, or define a clear override/extension pattern.

### 11. Undo/Redo Wiring Completeness

**Problem:** UndoRedoManager exists with command pattern and history panel, but
unclear how many editor operations actually create EditorCommands. Need audit
and wiring for transform gizmo, component edits, hierarchy changes.

### 12. LOD Auto-Generation

**Problem:** MeshLOD system (644 lines .cpp) has distance-based selection and
edge cost computation, but requires manually pre-made LOD chains. No automatic
mesh simplification from high-poly source.

**Recommendation:** Integrate meshoptimizer (already referenced in MeshOptimizer.h)
for automatic LOD chain generation.

### 13. Multi-Monitor / Floating Editor Windows

**Problem:** Editor uses ImGui dockspace with 40+ panels, but no floating window
support, no multi-monitor layout, no detachable panels. Layout isn't persisted.

### 14. Multiplayer Tutorial Documentation

**Problem:** "Making Your First Game" tutorial exists, but nothing for multiplayer.
Given the networking stack's sophistication, a tutorial would be high-value for adoption.

### 15. AI/NavMesh Visual Debugger

**Problem:** AI system is 5,952 lines with NavMesh, behavior trees, perception,
formation, cover — but no runtime visualization. Drawing NavMesh polygons,
perception cones, and BT state in viewport would make AI development practical.

---

## Game Module Status Summary

| Module | LOC | Subsystems | Playable? |
|--------|-----|-----------|-----------|
| SparkGame | 852 | 1 | No — demo only |
| SparkGameFPS | 21,746 | 5 | Near — needs assets, audio |
| SparkGameMMO | 9,940 | 18 | No — needs networking |
| SparkGameRPG | 4,663 | 9 | Partial — needs NPC AI, branching |
| SparkGameARPG | 3,224 | 8 | Partial — needs proc dungeons |
| SparkGameRTS | 3,287 | 8 | Partial — needs pathfinding AI |
| SparkGameRacing | 3,779 | 8 | Partial — needs advanced AI |
| SparkGamePlatformer | 4,079 | 8 | Partial — needs level design |
| SparkGameOpenWorld | 4,862 | 9 | Partial — needs world streaming |
| SparkGameVisualScript | 354 | 1 | No — shell only |

**Key finding:** FPS module is production-ready code (21.7K LOC) but no module uses
engine networking, physics integration is sparse, and audio is minimally integrated.

## Test Suite Quality Summary

| Category | Count | Status |
|----------|-------|--------|
| Total test files | 264 | Good |
| Integration tests | 6 | Minimal |
| E2E tests | 1 pseudo | Inadequate |
| Stress tests | 9 | Good |
| Cross-system tests | 0 | Missing |
| Systems without tests | ~10 | Gaps |

---


## Completion Update (2026-04-09)

All Tier 1 and Tier 2 recommendations listed in this document are now completed in the codebase:

1. **Cross-system integration tests** — covered by dedicated integration suites:
   - `TestCrossSystemIntegration.cpp`
   - `TestPhysicsECSIntegration.cpp`
   - `TestRenderECSIntegration.cpp`
   - `TestAnimationPhysicsIntegration.cpp`
   - `TestNetworkReplicationIntegration.cpp`
2. **Vulkan backend parity baseline** — Vulkan backend implementation and RHI shader compilation path are in place and CI-covered as experimental.
3. **Game module networking wiring** — FPS module contains real multiplayer integration with engine `NetworkManager`, including replication/prediction scaffolding and runtime commands.
4. **Real shader inventory for advanced rendering systems** — production HLSL/GLSL shader trees exist for raster, compute, mesh shader, and DXR pipelines.
5. **FBX import pipeline** — FBX importer is implemented and wired into `AssetPipeline::LoadFBX`.
6. **GPU profiling/frame analysis** — GPU profiling + timestamp systems are integrated and editor-visible.
7. **Shader hot-reload compilation** — hot reload compile path is wired to RHI shader compilation with diagnostics.

Tier 3 items are tracked as polish/maturity work and no longer block core production-readiness goals.

---

## Recommended Execution Order

1. **Integration tests** — Low risk, high confidence gain, validates existing systems
2. **Shader hot-reload compilation** — Small change, big developer experience win
3. **FBX import** — Unlocks professional artist workflows
4. **Vulkan backend** — Large effort but validates architecture
5. **Game module networking** — Proves the networking stack
6. **Real shaders for rendering systems** — Makes advanced visuals functional
7. Everything else by priority tier
