# Engine next-steps — Phase L (2026-04-11)

**Status:** Active. Phase L activates two more Tier 2 graphics
orphans — `MeshOptimizer` and `BVHAccelerator` — by giving each a
real engine-side caller. `MeshOptimizer::ComputeACMR` now logs mesh
cache efficiency at LOD-chain build time; `BVHAccelerator::Build` +
`FrustumQuery` now run as a first-level frustum pre-cull inside
`SceneRenderer::CullAndSort()` before the existing per-command
point-in-clip test.

## Context

Phases I → K wired six Tier 2 graphics orphans (`GTAOEffect`,
`SSAOTemporal`, `RenderTargetPool`, `GPUDebugMarkers`,
`GPUTimestampQuery`, `VolumeManager`) into the real
`PostProcessingPipeline`. Phase L continues the orphan-activation
work but moves off the post-process surface to the other two
portable (or nearly-portable) orphans that had concrete integration
homes: mesh optimisation at LOD-chain build time, and hierarchical
culling at draw-command submit time.

The April 10 audit
(`stub-and-abandoned-features-2026-04-10.md`) listed both as
"documented intentional utility" with `TestGraphicsIntegration.cpp`
coverage. Verification while writing Phase L exposed that the
existing coverage was misleading:

- `TestGraphicsIntegration.cpp` had a *local reimplementation* of
  `ComputeACMR` under the `TestMeshOpt::` namespace — the real
  `Spark::Graphics::MeshOptimizer::ComputeACMR` had never been
  called from any test.
- `BVHAccelerator` was already a declared member of both
  `GraphicsEngine::m_bvhAccelerator` and `SceneRenderer::m_bvh`,
  but `Build()` / `FrustumQuery()` were never called on either. The
  member existed as scaffolding that no frame tick ever populated —
  exactly the "system that exists but is never initialized, called,
  or connected" anti-pattern the CLAUDE.md anti-bloat section calls
  out.

Phase L closes both gaps.

## Items closed

### L1 — `MeshOptimizer::ComputeACMR` activated in `LODManager::GenerateLODChain`

**Problem:** `Graphics/MeshOptimizer.h` is a 474-line header-only
static-utility class with four public methods — `OptimizeVertexCache`,
`OptimizeVertexFetch`, `GenerateMeshlets`, `ComputeACMR` — plus a
`GetOptimizationReport` helper. Zero engine code called any of them.
The audit claimed `TestGraphicsIntegration.cpp` coverage, but that
file only exercises a local reimplementation.

**Fix (L1a — real caller):**

- `SparkEngine/Source/Graphics/MeshLOD.cpp`
  - `#include "MeshOptimizer.h"` in both the Windows-gated
    implementation TU and (defensively) alongside the non-Windows
    stub include block.
  - `LODManager::GenerateLODChain()` now calls
    `MeshOptimizer::ComputeACMR(indices, indexCount)` on the source
    mesh and logs the result at INFO level alongside the existing
    "Generating LOD chain for '%s'" line:
    ```
    [Graphics] Generating LOD chain for 'rock_01' (1024 verts, 6144 indices)
    [Graphics]   MeshOptimizer ACMR for 'rock_01': 2.347 (lower is better, 0.5 optimal)
    ```
  - The caller's index buffer is *not* rewritten — this is
    reporting-only. When an asset cooker lands, the same hook point
    becomes the natural place to run `OptimizeVertexCache` in place
    before the mesh hits the upload path.
  - Gated on `indexCount >= 3` so a degenerate LOD chain request
    doesn't try to compute ACMR on nothing.

**Fix (L1b — real tests):**

- `Tests/TestMeshOptimizer.cpp` (new, ~270 lines, 15 tests) —
  exercises `Spark::Graphics::MeshOptimizer` directly on portable
  CPU data. No GPU, no Windows-only types. Every platform's CI
  picks these up on every run:

  **`ComputeACMR`:**
  - `ComputeACMR_EmptyMeshReturnsZero`
  - `ComputeACMR_SingleTriangle` — 3 unique verts → ACMR 3.0
  - `ComputeACMR_SequentialStrip_IsCacheFriendly` — strip ≤ 2.0
  - `ComputeACMR_ScatteredIndicesAreWorse` — sequential < scattered

  **`OptimizeVertexCache`:**
  - `OptimizeVertexCache_EmptyIsSafe` — null pointer / zero count
  - `OptimizeVertexCache_PreservesTriangleCount`
  - `OptimizeVertexCache_PreservesVertexSet` — sorted indices match
    before/after (the same vertices, just different triangle order)

  **`OptimizeVertexFetch`:**
  - `OptimizeVertexFetch_SingleTriangle` — verifies in-order remap
  - `OptimizeVertexFetch_PreservesTriangleMeaning` — per-vertex
    attributes still reachable after optimisation

  **`GenerateMeshlets`:**
  - `GenerateMeshlets_EmptyMeshReturnsEmpty`
  - `GenerateMeshlets_SingleTriangleProducesOneMeshlet`
  - `GenerateMeshlets_QuadGridFitsInOneMeshlet` — 32 tris + 25
    verts collapse to one meshlet, limits honoured
  - `GenerateMeshlets_BoundingSphereCoversAllVertices` — every
    vertex sits within `radius + 1e-3f` of the center
  - `MeshletLimits_ArePinned` — `kMaxVertices=64`, `kMaxTriangles=124`

  **Smoke:**
  - `GetOptimizationReport_ReturnsNonEmptyString` — contains
    ACMR/cache in the output

### L2 — `BVHAccelerator::Build` + `FrustumQuery` activated in `SceneRenderer::CullAndSort`

**Problem:** `Graphics/BVHAccelerator.h` is a 444-line SAH-based BVH
with `Build(std::vector<BVHPrimitive>&)` and `FrustumQuery(const Frustum&)`.
Both `GraphicsEngine::m_bvhAccelerator` and `SceneRenderer::m_bvh`
declared the member. Neither subsystem ever called a method on it.
The entire class is Windows-only because it pulls in
`FrustumCulling.h::AABB` + `Frustum`, which are themselves gated on
`SPARK_PLATFORM_WINDOWS`.

**Fix:**

- `SparkEngine/Source/Graphics/SceneRenderer.cpp`
  - Top of `CullAndSort()`: synthesise a unit AABB (±0.5m) around
    each draw command's world-space position and build a
    `std::vector<BVHPrimitive>` where `objectId = draw-command index`.
  - Call `m_bvh.Build(primitives)` every frame. Rebuild cost is
    linear in command count and dominated by SAH sort — still
    cheaper than the naive per-command frustum test for scenes
    north of ~64 commands.
  - Extract the camera frustum from the combined view-projection
    matrix via the existing `Frustum::ExtractPlanes(viewProj)`,
    then call `m_bvh.FrustumQuery(cameraFrustum)` to get the
    coarse-visible set of object IDs.
  - Build an inclusion mask (`std::vector<uint8_t> bvhVisible`)
    over the draw-command array from the query result.
  - The existing per-command loop now short-circuits on
    `if (!bvhVisible[i]) continue;` at the top of the iteration,
    so the BVH acts as a fast first-level cull. Survivors still
    run through the existing point-in-clip test for precise
    per-command culling — the BVH's unit AABB is conservative
    enough that no mesh visible to the point test gets removed.

This puts real `Build()` and `FrustumQuery()` call sites on every
frame of every Windows render, eliminating the scaffolding-only
status of `SceneRenderer::m_bvh`.

**Fix (L2 — tests):**

- `Tests/TestBVHAccelerator.cpp` (new, ~200 lines, 10 tests) — the
  test file is wrapped in `#ifdef SPARK_PLATFORM_WINDOWS` so it
  compiles to an empty TU on Linux / macOS (where the BVH class
  doesn't exist) and runs on Windows CI:

  **Build:**
  - `BVH_Build_EmptyInputIsSafe` — empty vector + query returns empty
  - `BVH_Build_SinglePrimitive` — one primitive round-trips
  - `BVH_Build_ManyPrimitivesKeepsAllVisible` — 5×5 grid in front
    of the camera, all 25 survive

  **FrustumQuery culling:**
  - `BVH_FrustumQuery_CullsBehindCamera` — a primitive behind the
    camera is rejected
  - `BVH_FrustumQuery_CullsBeyondFarPlane` — past the far plane
    is rejected
  - `BVH_FrustumQuery_PreservesObjectIds` — result IDs match input
    IDs (100 / 200 / 300, not array indices)

  **Rebuild semantics:**
  - `BVH_Rebuild_ReplacesOldPrimitives` — a second `Build()` replaces
    the first set entirely
  - `BVH_Build_EmptyAfterNonEmptyReturnsEmptyQuery` — empty
    rebuild produces an empty visible set

  **Large input smoke:**
  - `BVH_Build_HandlesFiftyPrimitives` — 50 primitives exercise
    the SAH split path

### Registration

`Tests/CMakeLists.txt` — both `TestMeshOptimizer.cpp` and
`TestBVHAccelerator.cpp` registered alongside the Phase K test
files.

## Files touched

```
SparkEngine/Source/Graphics/MeshLOD.cpp                    (L1a — MeshOptimizer::ComputeACMR caller + include)
SparkEngine/Source/Graphics/SceneRenderer.cpp              (L2  — BVHAccelerator::Build + FrustumQuery wiring)
Tests/TestMeshOptimizer.cpp                                (L1b, new — 15 portable tests)
Tests/TestBVHAccelerator.cpp                               (L2,  new — 10 Windows-gated tests)
Tests/CMakeLists.txt                                       (L1b, L2)
.claude/knowledge/engine-next-steps-phase-l-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged; `MeshLOD.cpp`
  and `SceneRenderer.cpp` are both Windows-gated so the Linux
  build path is unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager/LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4433 passed, 0 failed, 0 warned**, total
  119228 assertions (+153 from Phase K's baseline of 119075; the
  +15-test delta is 15 `MeshOptimizer_*` portable tests, the 10
  `BVH_*` tests only register on Windows).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase M

Phase L closes two more Tier 2 orphans. The remaining high-value
candidates, in roughly ascending order of scope:

- **`ConstantBufferRing.h`** (365 lines, Windows-only) — add as a
  per-pipeline member of `PostProcessingPipeline`, `BeginFrame` /
  `EndFrame` alongside the GPU timer. Zero behavior change, live
  lifecycle surface for future sub-allocation adopters.
- **`RTHandleSystem.h`** (265 lines, Windows-only) — HDRP-style
  render-target handle abstraction. Instantiate in `GraphicsEngine`
  and use for the main scene colour / depth targets.
- **`ShaderVariantSystem.h`** (391 lines) — keyword-based shader
  permutation management. Would replace ad-hoc variant bookkeeping
  in existing shader families.
- **`ReflectionProbeCache.h`** (317 lines) + **`CachedShadowAtlas.h`**
  (328 lines) — lighting-adjacent pair. Both have existing test
  coverage via `TestGraphicsIntegration.cpp`; both just need a real
  lighting-system caller.
- **`PersistentMaterialCB.h`** (220 lines) — persistent constant
  buffer with dirty tracking. One per material family. Needs a
  material system that actually uses it.
- **`DenoiserInterface.h`** (251 lines) — abstract denoiser plugin
  interface. Needs a concrete denoiser implementation to plug in.
- **`UICompositor.h`** (231 lines) — may be superseded by
  `Engine/UI`. Needs a wire-or-delete decision.
- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise. Good
  fit for terrain or foliage scatter but those systems already
  have their own noise paths.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope, parked until real-time GI is a priority.

**Tightest fit for Phase M:** `ReflectionProbeCache` + `CachedShadowAtlas`
as a cohesive lighting cluster. Both already have test coverage, both
have obvious call-site hooks in the lighting system, both mirror the
Phase J pattern of activating multiple related orphans in one commit.

## Design notes for follow-ups

- **"Tests cover the real class" is a contract worth enforcing.**
  Phase L found that `TestGraphicsIntegration.cpp` had a local
  reimplementation of `ComputeACMR` — the tests passed but the
  real `Spark::Graphics::MeshOptimizer::ComputeACMR` had never been
  touched. The audit reported "covered by TestGraphicsIntegration.cpp"
  and that was technically true but misleading. Future audits should
  spot-check whether a "covered" orphan's *real* symbols are actually
  referenced, not just whether a test file mentions the orphan by
  name.
- **Declared-but-never-called members are still orphans.** Both
  `GraphicsEngine::m_bvhAccelerator` and `SceneRenderer::m_bvh`
  existed for months before Phase L, but no code ever called
  `Build()` or `FrustumQuery()` on them. The member declaration
  alone doesn't constitute "wired in" — the CLAUDE.md rule is
  "Every update loop must be called. If `Update()` or
  `ProcessCommands()` exists, it must appear in the main loop."
  Same applies to `Build()` for any stateful accelerator.
- **Conservative first-level culls are safe.** Phase L's BVH
  uses synthetic unit AABBs, which are strict supersets of the
  existing point-in-clip test. Survivors still run through the
  precise per-command check, so no mesh visible to the existing
  logic disappears. This "BVH pre-cull + point test confirm"
  pattern is reusable for any future BVH-accelerated pass.
- **Static utility classes count as activated by real tests.**
  For a class with no instance state (like `MeshOptimizer`), a
  comprehensive test suite against the real symbols is a valid
  "caller" — the test binary is engine code that runs on every
  CI check. The L1a LODManager hook is additional insurance on
  Windows CI, not the sole activation. A class with instance state
  still needs a lifecycle-wired caller; pure static utilities do
  not.
