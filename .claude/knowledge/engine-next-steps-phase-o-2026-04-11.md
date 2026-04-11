# Engine next-steps — Phase O (2026-04-11)

**Status:** Active. Phase O activates `Spark::Graphics::ShaderVariantSystem`
— a Tier 2 graphics orphan cataloged in the April 10 audit — by
making it a per-instance member of the `Shader` class with lifecycle
hooks on both the Windows and Linux implementation paths.

## Context

Phases I → N wired twelve Tier 2 graphics orphans across
`PostProcessingPipeline`, `LODManager`, `SceneRenderer`, and
`LightingSystem`. Phase O moves to the next obvious surface: the
`Shader` class — the central shader management entry point — as the
natural home for keyword and variant bookkeeping.

`Graphics/ShaderVariantSystem.h` (391 lines) is a complete keyword /
variant manager:

- Keyword registration with three keyword types (MultiCompile,
  ShaderFeature, DynamicBranch) and a `kMaxKeywords = 64` ceiling.
- Keyword groups for mutually-exclusive options (e.g.
  `_SHADOWS_OFF` / `_SHADOWS_LOW` / `_SHADOWS_HIGH`).
- A 64-bit `ShaderVariantKey` that treats each keyword as a bit so
  variants compose via bitwise OR.
- Variant resolution: `ResolveVariantKey(materialBits)` OR's
  material-level keywords onto the global state.
- `GetDefinesForVariant(key)` produces the preprocessor define
  list for a given variant.
- `StripUnusedVariants(usedKeywords)` reports how many ShaderFeature
  variants can be stripped at build time.
- `ShouldCompileVariant(key, disabledFeatures)` skips variants
  depending on disabled engine features (e.g. `_DXR_ENABLED` when
  DXR is off).
- `GetWarmupVariants(usedKeywords)` returns the list of variant
  keys that should be pre-compiled at shader load time.

No engine code instantiated it. `ShaderCrossCompiler.h`,
`ShaderDiskCache.h`, and `SlangShaderInterface.h` all referenced it
in `@see` comments but none owned an instance.

## Items closed

### O1 — `ShaderVariantSystem` activated as a per-`Shader` member

**Problem:** `ShaderVariantSystem` had no engine caller. It was
documented as "Covered by `Tests/TestGraphicsIntegration.cpp`" but
inspection showed the existing coverage was a mix of three small
unit tests against variant-key bit manipulation and a "max keywords"
constant pin — the class lifecycle, registration, groups, variant
resolution, stripping, and pre-warming paths had zero coverage.

**Fix:**

- `SparkEngine/Source/Graphics/Shader.h`
  - `#include "ShaderVariantSystem.h"` outside the Windows guard
    (the variant system is pure CPU).
  - New private member
    `Spark::Graphics::ShaderVariantSystem m_variantSystem;`
    alongside the existing shader-compilation state. The `Shader`
    class lives in the global `Spark` namespace (not
    `Spark::Graphics`), so the member and accessor signatures need
    explicit `Spark::Graphics::` qualification — the same pattern
    Phase M used for `LightingSystem`.
  - New public accessor pair `GetVariantSystem()` const/non-const.

- `SparkEngine/Source/Graphics/Shader.cpp`
  - **Windows `Shader::Initialize`** (line 112) calls
    `m_variantSystem.Initialize()` after creating the vertex /
    pixel shader resources.
  - **Windows `Shader::Shutdown`** (line 141) calls
    `m_variantSystem.Shutdown()` before resetting the D3D11
    device pointers.
  - **Linux `Shader::Initialize`** (line 492 — inside the
    `#else !SPARK_PLATFORM_WINDOWS` branch) mirrors the Windows
    hook after `CreateConstantBuffers()`.
  - **Linux `Shader::Shutdown`** (line 520) mirrors the teardown
    before clearing the device pointers.

Every `Shader` instance now has a live keyword / variant system.
Downstream callers — material system, shader hot-reload,
ShaderDiskCache — can register keywords on `shader.GetVariantSystem()`,
resolve variant keys, and query the define list without creating a
standalone instance or touching any singleton.

### Test coverage

**O-new: `Tests/TestShaderVariantSystem.cpp`** (~300 lines, 21 tests)

Exercises the real `Spark::Graphics::ShaderVariantSystem` directly.
No GPU, no shader compilation, no D3D11 — pure CPU keyword math.
Every test runs on every platform's CI:

- **Lifecycle:** `InitializeAndShutdown`,
  `ReinitializeClearsState`.
- **Keyword registration:** `RegisterKeywordAssignsSequentialIndices`,
  `RegisterKeywordWithDefaultEnabled` (default-on keywords toggle
  the global state bit), `GetKeywordByIndex` (with bounds checks),
  `GetKeywordIndexUnknownIsNegativeOne`,
  `RegisterKeywordGroupAllocatesContiguous` (3-keyword group with
  default index 1 → middle keyword enabled).
- **Keyword state management:** `SetGlobalKeywordToggles`,
  `SetGlobalKeywordUnknownIsNoop`,
  `GetGlobalVariantKeyReflectsBits` (three keywords → three bits).
- **Variant key operations:** `VariantKeyEquality` (`==`, `!=`, `<`),
  `VariantKeyHasKeyword`, `VariantKeySetKeyword`.
- **Variant resolution:** `ResolveVariantKeyOrsGlobalAndMaterial`
  — global state + material-level overrides combine via bitwise OR;
  `GetDefinesForVariantReturnsEnabled` — only enabled keyword
  symbols appear in the define list.
- **Variant counting + stripping:** `GetTotalVariantCount` — 2^N
  for N keywords, 1 for zero;
  `StripUnusedShaderFeatures` — MultiCompile keywords are never
  stripped, ShaderFeatures only strip when not in `usedKeywords`;
  `ShouldCompileVariantSkipsDisabledFeatures` — variant with
  `_DXR_ENABLED` rejected when DXR is in the disabled set.
- **Pre-warming:** `GetWarmupVariantsIncludesDefaultAndToggles` —
  default + one toggle per MultiCompile keyword.
- **Console status:** `Console_GetStatusFormatting` (status string
  includes keyword names, ON/OFF flags).
- **kMaxKeywords limit:** `ExceedingKMaxKeywordsReturnsNegative` —
  exactly 64 keywords register successfully, the 65th returns -1.

### Registration

`Tests/CMakeLists.txt` — `TestShaderVariantSystem.cpp` registered
alongside the Phase N test files.

## Files touched

```
SparkEngine/Source/Graphics/Shader.h                         (O1 — include + member + accessor)
SparkEngine/Source/Graphics/Shader.cpp                       (O1 — Windows + Linux lifecycle hooks)
Tests/TestShaderVariantSystem.cpp                            (O1, new — 21 portable tests)
Tests/CMakeLists.txt                                         (O1)
.claude/knowledge/engine-next-steps-phase-o-2026-04-11.md    (new)
.claude/index.md                                             (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4505 passed, 0 failed, 0 warned**, total
  119495 assertions (+129 from Phase N's baseline of 119366; the
  +21-test delta is from 21 `ShaderVariantSystem_*` tests. Notable
  — the previously-flaky `LoadTest_Severe_EntityFlood` also passed
  cleanly this run).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase P

Phase O closes one more Tier 2 orphan, bringing the running total
across I→O to **thirteen orphans activated**. Remaining candidates,
in roughly ascending order of scope:

- **`PersistentMaterialCB.h`** (220 lines) — persistent constant
  buffer with dirty tracking. One per material family. Integration
  surface is `MaterialSystem` (exists somewhere in Graphics?) or a
  per-material wrapper.
- **`DenoiserInterface.h`** (251 lines) — abstract denoiser plugin
  interface for RT AO / GI / reflections. Needs a concrete denoiser
  implementation to plug in, so activation is harder than a
  straight lifecycle wire.
- **`UICompositor.h`** (231 lines) — may be superseded by
  `Engine/UI`. Needs a wire-or-delete decision after an audit of
  the current UI path.
- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise. Good
  fit for terrain or foliage scatter, but existing systems have
  their own noise generators — activation would be a parallel
  utility, not a replacement.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope, parked until real-time GI is a priority.

**Tightest fit for Phase P:** `PersistentMaterialCB` — if a
`MaterialSystem` or equivalent class exists with a clean hook
point, this is a direct wire-up. If not, the Phase P scope could
pivot to `DenoiserInterface` (needs a null denoiser factory
registration inside `GraphicsEngine`) or to the Graphics-folder
audit that decides `UICompositor`'s wire-or-delete fate.

## Design notes for follow-ups

- **Shaders now carry their own keyword state.** Every `Shader`
  instance has a `ShaderVariantSystem` that tracks the keywords it
  cares about. This is the per-shader-family pattern the audit
  called out ("A future material system will instantiate one per
  shader family"). Material systems, hot-reload, and disk cache
  should all query `shader.GetVariantSystem()` instead of
  maintaining their own keyword registries.
- **Global-namespace classes need explicit namespace qualification.**
  The `Shader` class lives in the global `Spark` namespace, not
  `Spark::Graphics`. New members of types from `Spark::Graphics::`
  must be fully qualified in both the header and accessor
  signatures — same gotcha Phase M hit on `LightingSystem`. Future
  phases that wire a `Spark::Graphics::` orphan onto a global-
  namespace or `Spark::` class should watch for this.
- **"Existing coverage" claims are worth verifying.** Phase L found
  that `TestGraphicsIntegration.cpp`'s `MeshOptimizer` coverage was
  actually a local reimplementation. Phase O found a similar
  issue: the existing coverage of `ShaderVariantSystem` was a thin
  3-test smoke at best, not the comprehensive coverage the audit
  implied. When a knowledge entry says an orphan is "covered by
  TestGraphicsIntegration.cpp", grep the real class name in that
  file and spot-check how many call sites you find. Often the
  answer is "one or two, against unrelated static methods" — a
  Phase-level activation should write real tests against the
  claimed public API.
- **Lifecycle hooks on both Windows and Linux paths are now the
  default for portable orphans.** Phases K, M, and O all wire
  portable orphans into subsystems that have both Windows
  implementation paths and Linux stub paths, and all three mirror
  the hook calls on both sides. This keeps tests that exercise the
  parent class on Linux CI consistent with the Windows production
  tick. Future orphan activations on similar bifurcated subsystems
  should follow the same pattern.
