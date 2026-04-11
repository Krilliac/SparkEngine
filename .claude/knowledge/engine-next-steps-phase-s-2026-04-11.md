# Engine next-steps — Phase S (2026-04-11)

**Status:** Active. Phase S activates
`Spark::Graphics::FastNoise2SIMD` — a 749-line SIMD-accelerated
procedural noise library with a composable node graph — by wiring
it as a `unique_ptr<NoiseGraph>` member of
`Spark::Graphics::GraphicsEngine` with a default `SimplexNode`
output on both the Windows real impl and the Linux stub path.

## Context

Phases I → R wired sixteen Tier 2 graphics orphans across
`PostProcessingPipeline`, `LODManager`, `SceneRenderer`,
`LightingSystem`, `Shader`, `MaterialSystem`, `GraphicsEngine`,
and `UISystem`. Phase S picks up `FastNoise2SIMD.h`, one of the
two remaining Tier 2 orphans with a clear activation path.

`Graphics/FastNoise2SIMD.h` (749 lines) is the largest orphan to
activate so far. The header bundles:

- `SIMDLevel` enum (`Scalar` / `SSE2` / `AVX2`) + runtime
  `DetectBestSIMD()` detection + `SIMDLevelName()` helper.
- `NoiseNode` abstract base with `Evaluate(x, y)` and
  `BatchEvaluate(out, inX, inY, count)` (scalar path processes
  4 values per iteration for consistent batch semantics).
- Concrete node types: `SimplexNode` (simplex lattice),
  `PerlinNode` (classic gradient noise), `CellularNode`
  (Worley / cellular noise), `FBMNode` (fractional Brownian
  motion with configurable octaves / lacunarity / gain),
  `DomainWarpNode` (noise-source coordinate warping),
  `CombinerNode` (Add / Multiply / Min / Max of two child
  nodes).
- `NoiseGraph` owning container with
  `std::vector<std::unique_ptr<NoiseNode>>` node storage, raw
  output pointer, and `AddNode` / `SetOutputNode` /
  `Evaluate` / `BatchEvaluate` / `GetNodeCount` /
  `GetSIMDLevel` accessors.

All pure CPU. No D3D11. The runtime SIMD detection falls back to
the scalar path when neither SSE2 nor AVX2 is available, so the
header compiles and runs on every platform.

## Items closed

### S1 — `NoiseGraph` activated as a `GraphicsEngine` member

**Problem:** `FastNoise2SIMD.h` had no engine caller. The audit
noted the class was "kept self-contained so neighbouring code
can use it without a compiled dependency" — an explicit
intentional-utility marker — but zero callers meant the SIMD
kernel and node graph were untested through the real class path
and effectively dead in the engine.

**Fix:**

- `SparkEngine/Source/Graphics/GraphicsEngine.h`
  - `#include "FastNoise2SIMD.h"` alongside the other post-
    process / RHI includes. Portable — no Windows guard.
  - New private member
    `std::unique_ptr<Spark::Graphics::NoiseGraph> m_proceduralNoise;`
    immediately after the Phase Q `m_denoiser` member. Owned
    by `unique_ptr` because `NoiseGraph` holds a
    `std::vector<std::unique_ptr<NoiseNode>>` and the raw
    output pointer points *into* that vector — moving or
    copying the graph would invalidate the output pointer, so
    heap ownership keeps the invariants stable.
  - New public accessor pair `GetProceduralNoise()`
    const/non-const (returns raw `NoiseGraph*`). Terrain,
    foliage scatter, and procedural decoration systems can
    call `Evaluate(x, y)` / `BatchEvaluate(...)` or
    `AddNode(std::make_unique<...>)` through the accessor.
  - New `GetProceduralNoiseSIMDLevel()` accessor — returns the
    runtime-detected `SIMDLevel` enum so callers can branch on
    expected performance characteristics.

- `SparkEngine/Source/Graphics/GraphicsEngine.cpp`
  - **Windows `Initialize`** (right after the Phase Q denoiser
    activation) allocates a `NoiseGraph` via `std::make_unique`,
    constructs a default `SimplexNode` through
    `std::make_unique<SimplexNode>`, passes it to
    `m_proceduralNoise->AddNode(std::move(defaultNode))` (which
    takes ownership and returns a raw pointer), and wires the
    returned pointer as the graph's output via
    `SetOutputNode(nodePtr)`. The graph is ready to evaluate
    Simplex noise as soon as Initialize returns.
  - **Windows `Shutdown`** calls `m_proceduralNoise.reset()`
    alongside the Phase Q `m_denoiser.reset()`. The unique_ptr
    destructor releases all owned nodes via the graph's
    internal vector destructor.
  - **Linux stub `Initialize` / `Shutdown`** mirror the same
    allocation / teardown sequence so headless builds see the
    same default output node and tests exercising
    `GraphicsEngine` directly get consistent state on every
    platform.
  - New `GetProceduralNoiseSIMDLevel()` bodies — identical
    one-liners forwarding to `Spark::Graphics::DetectBestSIMD()`
    — in **both** branches of the `.cpp` file (Phase Q hit the
    same duplication gotcha for `GetDenoiserBackend()`).

Every `GraphicsEngine` instance now has a live procedural noise
graph with a default Simplex output. Terrain generation, foliage
scatter, decoration placement, and any other procedural system
can call `graphicsEngine->GetProceduralNoise()->Evaluate(x, y)`
or compose more complex graphs via `AddNode` without creating a
standalone instance.

### Test coverage

**S-new: `Tests/TestFastNoise2SIMD.cpp`** (~400 lines, 28 tests)

Exercises the real `Spark::Graphics::FastNoise2SIMD` family
directly. No GPU, no D3D11, no external SDK — pure CPU noise
math. Every test runs on every platform's CI:

- **SIMD detection (3 tests):** `DetectBestSIMDReturnsValidLevel`
  (level is in [0, 2]), `SIMDLevelNameIsNonNull` (every enum
  value + out-of-range returns non-null), `NoiseGraphReportsSIMDLevel`
  (the static graph accessor matches the free function).

- **SimplexNode (4 tests):** `SimplexNodeProducesBoundedOutput`
  (|v| <= 2 across an 8x8 sample grid),
  `SimplexNodeIsDeterministic` (same input → same output),
  `SimplexNodeDifferentSeedsDiffer` (at least one sample
  across an 8-point scan differs between seeds),
  `SimplexNodeTypeIsSimplex`.

- **PerlinNode (2 tests):** `PerlinNodeIsDeterministic`,
  `PerlinNodeTypeIsPerlin`.

- **CellularNode (2 tests):** `CellularNodeIsDeterministic`,
  `CellularNodeTypeIsCellular`.

- **FBMNode (3 tests):** `FBMNodeHonoursOctaves` (4 octaves /
  lacunarity 2.0 / gain 0.5 round-trip through the accessors),
  `FBMNodeIsDeterministic`, `FBMNodeTypeIsFBM`.

- **DomainWarpNode (3 tests):**
  `DomainWarpAmplitudeAccessors` (get/set round-trip),
  `DomainWarpTypeIsDomainWarp`,
  `DomainWarpNullChildIsSafe` (null source + warpNoise
  returns 0.0f).

- **CombinerNode (5 tests):** `CombinerAddOp` (sum of two
  child Evaluates), `CombinerMultiplyOp` (product),
  `CombinerMinMaxOps` (both Min and Max against `std::min` /
  `std::max` of the same children), `CombinerNullChildrenIsSafe`,
  `CombinerSetOp` (SetOp / GetOp round-trip).

- **Batch evaluation consistency (2 tests):**
  `BatchEvaluateMatchesSingleEvaluate` (13-sample batch — not
  a multiple of 4 so the remainder loop runs — matches the
  single-sample Evaluate path),
  `FBMBatchMatchesSingle` (8-sample FBM batch matches the
  per-sample Evaluate results within a larger epsilon because
  of the normalisation mul-add ordering).

- **NoiseGraph (5 tests):** `NoiseGraphEmptyEvaluatesZero`
  (no output node + no nodes → Evaluate returns 0),
  `NoiseGraphAddsNodeAndRoutes` (AddNode returns a raw
  pointer, SetOutputNode wires it, and Evaluate matches the
  node's direct Evaluate),
  `NoiseGraphBatchEvaluateWithoutOutputIsZero` (missing output
  zero-fills the batch output buffer),
  `NoiseGraphBatchMatchesOutputNode` (graph batch path matches
  direct node batch path),
  `NoiseGraphOwnsMultipleNodes` (3-node graph grows
  `GetNodeCount` correctly).

### Registration

`Tests/CMakeLists.txt` — `TestFastNoise2SIMD.cpp` registered
alongside the Phase R test files.

## Files touched

```
SparkEngine/Source/Graphics/GraphicsEngine.h               (S1 — include + member + accessors)
SparkEngine/Source/Graphics/GraphicsEngine.cpp             (S1 — Windows + Linux lifecycle hooks)
Tests/TestFastNoise2SIMD.cpp                               (S1, new — 28 portable tests)
Tests/CMakeLists.txt                                       (S1)
.claude/knowledge/engine-next-steps-phase-s-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4587 passed, 0 failed, 0 warned** (even
  the normally-flaky `LoadTest_Severe_EntityFlood` passed cleanly
  this run), total 120370 assertions (+134 from Phase R's
  baseline of 120236; the +28-test delta is the new
  `FastNoise2SIMD_*` file).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase T

Phase S closes one more Tier 2 orphan, bringing the running total
across I→S to **seventeen orphans activated**. Only one major
candidate remains from the audit's Tier 2 list:

- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope. The class is tightly coupled to a real
  GI render pipeline (requires a 3D voxel grid, cone-march
  compute shader, voxelization pass against the scene's
  meshes). Activation without an actual GI render path would
  mean "lifecycle only" in the Phase N `ConstantBufferRing`
  pattern — allocate the grid, initialise the settings, tear
  it down on Shutdown, and defer the compute-shader wiring to
  a follow-up phase.

After Phase T (if that's the path chosen), the Tier 2 graphics
orphan pool catalogued in
`stub-and-abandoned-features-2026-04-10.md` will be essentially
exhausted. Future phases should either:

1. **Move on to Tier 3 / Tier 4 orphans** — the Engine-
   non-Graphics orphans (`Tier 3: tested-but-unwired`,
   `Tier 4: editor infrastructure with unimplemented bodies`)
   that the April 10 audit catalogued.
2. **Revisit Tier 2 items marked as false positives.** Several
   Graphics-folder files were skipped during the audit because
   they turned out to already be wired or were filename
   collisions. A second pass might reveal items worth
   re-activating at higher quality.
3. **Start a new roadmap.** Two candidate themes:
   - The "wire-but-shallow" cleanup — orphan *consumers* that
     call one method on an activated orphan but don't
     exercise the full public API. Phase L's
     `BVHAccelerator` wiring is a good example: the BVH is
     now built every frame inside `SceneRenderer::CullAndSort`
     but only the raw primitive AABBs are fed in, not real
     mesh bounds — a follow-up could improve the quality.
   - The "shader hot-reload surface" — several Phase-
     activated orphans (Shader variant system, material CB,
     procedural noise) have lifecycle but no connection to
     the runtime reload path. Wiring those surfaces to
     `ShaderHotReload` would give live-editing a bigger
     integration footprint.

## Design notes for follow-ups

- **`unique_ptr<OwnedNodes>` beats stack-allocated graphs when
  raw pointers alias into container storage.** Phase S's
  `NoiseGraph` holds a `std::vector<std::unique_ptr<NoiseNode>>`
  and the graph's `m_outputNode` raw pointer points *into* the
  vector. A stack-allocated graph would work, but
  moving / copying it (e.g. via a `GraphicsEngine` copy or a
  std::vector<NoiseGraph> resize) would invalidate the output
  pointer. Heap ownership via `unique_ptr` makes the alias
  safe in perpetuity because the graph never moves.
- **Default output nodes make the accessor useful
  immediately.** Phase S constructs a default `SimplexNode` in
  `GraphicsEngine::Initialize` so callers can
  `Evaluate(x, y)` without setting up their own graph first.
  This is the same pattern Phase Q used for the denoiser
  (`SoftwareDenoiser` default). Future `unique_ptr` orphan
  activations should construct a working default backend in
  `Initialize` so the accessor path is live from frame 1.
- **Runtime SIMD detection is essentially free on x86.** The
  `DetectBestSIMD()` function compiles down to a constant fold
  on every modern compiler because the check is preprocessor-
  based (`__AVX2__` / `__SSE2__` macros), not runtime `cpuid`.
  This means `GetProceduralNoiseSIMDLevel()` is a zero-cost
  accessor — callers can branch on it without worrying about
  repeated detection overhead.
- **Tests against batch APIs should use non-multiple-of-4
  counts.** Phase S's `BatchEvaluateMatchesSingleEvaluate` test
  uses 13 samples specifically so the `count - count%4 = 12`
  unrolled loop runs first and the remaining 1 sample hits
  the scalar cleanup path. Both branches need test coverage or
  the scalar fallback path regresses silently. Future batch-
  API tests (if `CachedShadowAtlas` or `ReflectionProbeCache`
  grow batch methods) should follow the same pattern.
- **`enum class` casts stay in tests.** Phase Q added the
  `static_cast<int>` workaround for `EXPECT_EQ(enum_class_val,
  enum_class_val)` because the test framework streams both
  operands to `std::ostream` for diagnostics. Phase S hits
  the same pattern in its `GetType()` checks against
  `NoiseNodeType::*` values and its `GetOp()` checks against
  `CombineOp::*`. The pattern is now well-established — every
  future phase testing `enum class` values should use the
  cast upfront.
