# Engine next-steps — Phase R (2026-04-11)

**Status:** Active. Phase R activates
`Spark::Graphics::UICompositor` as a per-instance member of
`Spark::UI::UISystem` with `Initialize` / `OnResize` / `Render`
lifecycle hooks. This resolves the "wire or delete?" open
question the April 10 audit raised for this orphan.

## Context

Phases I → Q wired fifteen Tier 2 graphics orphans across
`PostProcessingPipeline`, `LODManager`, `SceneRenderer`,
`LightingSystem`, `Shader`, `MaterialSystem`, and `GraphicsEngine`.
Phase R tackles the first orphan that needed a decision before
activation: `UICompositor` was flagged by the audit as *may be
superseded by `Engine/UI`*. The correct path — wire or delete —
required reading the existing UI subsystem to see whether it
already had a compositor surface.

### The wire-or-delete audit

`Graphics/UICompositor.h` (245 lines) is a composition-stack
manager for nested widget effects:

- `CompositeOp` enum — None, AlphaBlend, Multiply, Screen, Mask,
  GaussianBlur.
- `CompositeRequest` — op + opacity + blur radius + target size.
- `UICompositor` class — maintains a pool of
  `PooledRenderTarget` metadata entries and a composition stack
  with `kMaxStackDepth = 8` and `kMaxPoolSize = 16`. Workflow:
  `BeginComposite(request)` acquires an intermediate RT slot,
  `EndComposite()` releases it. `BeginFrame()` reclaims stale
  entries that have been idle for 10+ frames.

`Engine/UI/UISystem.h` (453 lines) manages the widget hierarchy
(`UICanvas` root, `UIPanel` containers, `UIButton` / `UILabel` /
`UIProgressBar` / `UIImageWidget` leaves), layout, anchoring,
input dispatch, and visibility toggling.

A grep across `Engine/UI/` found **zero references** to
`UICompositor`, `CompositeOp`, `CompositeRequest`, or the
`BeginComposite` / `EndComposite` entry points. The existing
UI system has no compositor or per-widget compositing surface
of any kind. The audit's "may be superseded" note was wrong —
the two systems are complementary:

- `UISystem` manages the widget hierarchy, layout, and input.
- `UICompositor` manages the composition stack for nested widget
  effects (blur, mask, opacity) and pools the intermediate
  render targets those effects need.

**Decision: wire in.** `UICompositor` fills a gap in the existing
UI path rather than duplicating functionality.

## Items closed

### R1 — Fixed missing `<algorithm>` include in `UICompositor.h`

**Problem:** The header uses `std::remove_if` inside
`UICompositor::BeginFrame` but never included `<algorithm>`.
Compilation survived only because no engine code had ever
included the header directly — transitive includes from
consumers would have made this fail loudly. Phase R adds the
header as a consumer and needed the fix first.

**Fix:** Added `#include <algorithm>` to `UICompositor.h`
alongside the existing `<cstdint> / <string> / <vector>`
includes.

### R2 — `UICompositor` activated as a `UISystem` member

**Problem:** No engine caller. The audit's "may be superseded"
note was incorrect — no existing `Engine/UI` code provided the
composition-stack functionality.

**Fix:**

- `SparkEngine/Source/Graphics/UICompositor.h`
  - Rewrote the header `@note` from "may be superseded by
    `Engine/UI`. Kept for reference until..." to a concrete
    description of the Phase R activation, documenting that
    `Spark::UI::UISystem` now owns the instance and the wire-
    or-delete audit conclusion: the existing UI system has
    no compositor surface, so the two classes are
    complementary.

- `SparkEngine/Source/Engine/UI/UISystem.h`
  - `#include "../../Graphics/UICompositor.h"` alongside the
    existing UI widget includes.
  - New private member
    `Spark::Graphics::UICompositor m_compositor;` alongside
    the existing `UICanvas m_canvas` and `bool m_visible`.
    `UISystem` lives in `Spark::UI` (not `Spark::Graphics`),
    so the member type needs explicit namespace qualification —
    same gotcha Phases M, O, and P hit on `LightingSystem`,
    `Shader`, and `MaterialSystem`.
  - New public accessor pair `GetCompositor()` const/non-const.
    Future widget code can push composition levels through
    this accessor without duplicating the lifecycle logic.

- `SparkEngine/Source/Engine/UI/UISystem.cpp`
  - **`Initialize(screenWidth, screenHeight)`** clamps non-
    positive dimensions to 1 and calls
    `m_compositor.Initialize(w, h)` after the existing
    `m_canvas.Initialize` call. The clamp matches the existing
    UISystem warning behaviour — the caller gets the
    `SPARK_WARN_IF` + a still-valid compositor.
  - **`Render()`** calls `m_compositor.BeginFrame()` at the top
    of the method, *before* the visibility check. This is
    deliberate: the pool-reclaim path (evict entries idle for
    10+ frames) should drain state even when the UI is hidden,
    so toggling `SetVisible(false)` for a long time doesn't
    leak pool memory.
  - **`OnResize(width, height)`** calls
    `m_compositor.Initialize(w, h)` (with the same clamp) after
    the existing `m_canvas.Resize` call. `Initialize` doubles
    as a reset — clears the pool and the stack — so the
    resize path drops any in-flight composite levels and
    re-aligns the metadata with the new screen size.

Every `UISystem` instance now has a live compositor. A future
widget render pass can push a `CompositeRequest` (opacity,
blur, mask) for any nested widget that needs an intermediate
render target, and the pool will recycle the underlying slots
across frames.

### Test coverage

**R-new: `Tests/TestUICompositor.cpp`** (~230 lines, 13 tests)

Exercises the real `Spark::Graphics::UICompositor` directly.
Every test runs on every platform's CI:

- **Lifecycle:** `InitializeAndShutdown`,
  `BeginCompositeBeforeInitializeFails`,
  `EndCompositeOnEmptyStackIsSafe` (idempotent pop).
- **Push / pop:** `BeginCompositeAllocatesPoolEntry`,
  `BeginCompositeUsesScreenSizeWhenZero` (zero-width/height
  request falls back to the screen size),
  `NestedCompositeLevelsStack` (3-level nested push / pop with
  active target tracking), `MaxStackDepthLimit` (exactly 8
  succeed, 9th fails without polluting the stack).
- **Pool reuse:** `PoolReusesReleasedTargets` (same-size push
  reuses released slot, pool stays at size 1),
  `PoolAllocatesNewSlotForDifferentSize` (different size grows
  to 2), `MaxPoolSizeLimit` (16 distinct sizes succeed, 17th
  fails).
- **Frame reclaim:** `BeginFrameReclaimsStaleEntries` (15
  idle frames drain the pool past the 10-frame threshold),
  `BeginFrameClearsStack` (pushes without pops get cleaned up
  by the next BeginFrame).
- **Console status:** `Console_GetStatusFormatting` (status
  string includes pool, active, and depth counts).

**R-new: `Tests/TestUISystemPhaseR.cpp`** (~130 lines, 7 tests)

Portable integration tests against the real `Spark::UI::UISystem`:

- `PhaseR_Compositor_InitializedWithUISystem` — compositor
  follows UI system Initialize.
- `PhaseR_Compositor_HandlesNonPositiveDimensions` — 0/0
  dimensions clamp to 1/1.
- `PhaseR_Compositor_AcceptsPushPopViaAccessor` — push / pop
  through `GetCompositor()`.
- `PhaseR_Render_ReclaimsStaleCompositorEntries` — 15
  `ui.Render()` calls drain an idle pool entry (exercises the
  BeginFrame wiring through the Render hook).
- `PhaseR_Render_ClearsLeftoverStack` — mid-frame bail-out
  with unpopped composites gets cleaned up by the next
  `ui.Render()`.
- `PhaseR_OnResize_ReinitializesCompositor` — resize drops
  the pool and re-initialises at new dimensions.
- `PhaseR_OnResize_HandlesZeroDimensions` — 0/0 resize
  clamps to 1/1.

### Registration

`Tests/CMakeLists.txt` — both new files registered alongside
the Phase Q test files.

## Files touched

```
SparkEngine/Source/Graphics/UICompositor.h                (R1 — missing <algorithm> include; R2 — @note rewrite)
SparkEngine/Source/Engine/UI/UISystem.h                   (R2 — include + member + accessor)
SparkEngine/Source/Engine/UI/UISystem.cpp                 (R2 — Initialize / Render / OnResize hooks)
Tests/TestUICompositor.cpp                                (R2, new — 13 portable CPU tests)
Tests/TestUISystemPhaseR.cpp                              (R2, new — 7 portable integration tests)
Tests/CMakeLists.txt                                      (R2)
.claude/knowledge/engine-next-steps-phase-r-2026-04-11.md (new)
.claude/index.md                                          (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4557 passed, 0 failed, 1 warned**
  (pre-existing `LoadTest_Severe_EntityFlood` known-flaky),
  total 120236 assertions (+82 from Phase Q's baseline of
  120154; the +20-test delta is 13 `UICompositor_*` + 7
  `PhaseR_*` integration tests).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase S

Phase R closes the first orphan that required a pre-activation
decision, bringing the running total across I→R to **sixteen
orphans activated**. Remaining candidates:

- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise.
  Existing terrain / foliage systems have their own noise
  generators, so activation would make this an optional
  parallel backend, not a replacement. Natural home is a
  public `GraphicsEngine::GetProceduralNoise()` accessor with
  a `SIMDType` toggle, following the Phase Q `unique_ptr<IDenoiser>`
  pattern. Tests cover the SIMD math directly against
  reference CPU implementations.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope; the class is tightly coupled to a real
  GI render pipeline. Activation without an actual GI path
  would mean lifecycle-only (Phase N's `ConstantBufferRing`
  pattern), with a follow-up phase to wire the actual cone-
  trace compute shader and voxelization pass.

**Tightest fit for Phase S:** `FastNoise2SIMD` — it has a clean
public API (per-octave frequency, gain, lacunarity parameters),
pure CPU implementation, no render-pipeline dependency, and
tests can pin the noise output against reference tables to
ensure the SIMD path stays consistent with the scalar reference.

## Design notes for follow-ups

- **Audit "superseded" claims need a grep before activation.**
  Phase R's first move was to grep `Engine/UI/` for
  `UICompositor` / `CompositeOp` / `BeginComposite` / similar
  symbols. The answer was "zero references" — the audit's
  "may be superseded" note was incorrect. Every future phase
  that inherits a wire-or-delete decision from the audit
  should run the same grep first. Superseded by X requires
  "X has the same API" or "X solves the same problem" — not
  just "X exists in the same folder."
- **Hidden missing-include bugs surface on first consumer.**
  `UICompositor.h` used `std::remove_if` without including
  `<algorithm>`. The file had zero direct consumers so
  transitive includes from whatever tests pulled it in
  silently saved it. The moment Phase R added the first real
  consumer (`UISystem.h`), the missing include would have
  failed the build — but because `<algorithm>` was already
  transitively included elsewhere in the UI translation unit,
  the bug stayed invisible. Fix was a one-line include add.
  Future orphan activations should proactively add missing
  includes to the orphan header rather than rely on transitive
  luck.
- **Lifecycle hooks should respect visibility toggles.** The
  Phase R `Render()` implementation calls
  `m_compositor.BeginFrame()` *before* the `if (m_visible)`
  check, specifically so the pool-reclaim path drains even
  when the UI is hidden. Otherwise `SetVisible(false)` would
  leak pool memory indefinitely. Future orphan activations
  that hook into visibility-guarded render paths should check
  whether they need to run regardless of visibility
  (reclaim / eviction paths do; rendering paths do not).
- **`Initialize` doubles as reset for pure-CPU caches.**
  `UICompositor::Initialize(w, h)` clears the pool + stack and
  sets the reference size. That makes it a natural choice for
  `OnResize` rather than needing a separate `Resize()` method
  on the compositor. Similar pattern appears in Phase K's
  `VolumeManager::Initialize()` and Phase P's
  `PersistentMaterialCBManager::Initialize()`. Future orphans
  with `Initialize(params)` that doesn't allocate GPU state
  can usually be called from resize paths too.
