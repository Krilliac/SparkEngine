# Engine next-steps — Phase DD (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Wire `Spark::AI::AIDebugRenderer` singleton into
`GameplayLifecycleShared.cpp` and ship real-class tests for both it
and `Spark::UI::DirtyRegionGrid`. Theme 3D continuation.

---

## Phase DD targets

A third-pass sweep (`classes with void Initialize() but zero external
references`, excluding interfaces / math utilities / platform stubs)
surfaced two more orphans:

1. **`Spark::AI::AIDebugRenderer`** — `Engine/AI/AIDebugRenderer.h`
   (239 LOC) + `.cpp`. Singleton that manages AI debug overlay state
   (NavMesh, paths, perception cones, behavior-tree state, cover
   points, formations). Full `Initialize` / `Update` / `Shutdown`
   lifecycle plus `SetEnabled` / `SetOptions` / `SetSelectedAgent`
   configuration. The existing `Tests/TestAIDebugRenderer.cpp` is a
   **fake-coverage** test — it defines its own `TestAIDbg::XMFLOAT3`
   / `DebugDrawOptions` in an anonymous namespace and never touches
   the real class.

2. **`Spark::UI::DirtyRegionGrid`** — `Engine/UI/UIDirtyTracking.h`
   (307 LOC, header-only). ThorVG-inspired 16x16 screen grid tracker
   for UI dirty-region rendering. Pure CPU, no GPU dependency. **Not
   a singleton** — designed to be instantiated per-UI-system. No
   test coverage at all before Phase DD.

## Wire-up

### `AIDebugRenderer`

Added to `GameplayLifecycleShared.cpp`:

```cpp
// Initialize block (RenderingAndUtility section):
Spark::AI::AIDebugRenderer::GetInstance().Initialize();

// Shutdown block:
Spark::AI::AIDebugRenderer::GetInstance().Shutdown();
```

After Initialize the singleton is present but `IsEnabled()` is
false — the overlay draws nothing until an ImGui menu or debug
console toggles `SetEnabled(true)`. `Update(dt)` is pumped by the
AI system when debug mode is active; Phase DD only lands the
lifecycle seam.

### `DirtyRegionGrid`

**No lifecycle wire-up.** The class is per-UI, not a singleton —
any future UI subsystem that adopts dirty-region rendering will
own its own `DirtyRegionGrid` instance. Phase DD ships real-class
tests so the class is ready for adoption (any future caller can
trust the public contract).

## Files touched

### Code

- `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp`:
  - `#include "Engine/AI/AIDebugRenderer.h"`
  - `Initialize` block adds `AIDebugRenderer::GetInstance().Initialize()`.
  - `Shutdown` block releases the singleton first (before the
    Phase CC pair).

### Tests

- `Tests/TestAIDebugRendererPhaseDD.cpp` (new, 9 tests) covering:
  - Singleton accessor stability.
  - Initialize/Shutdown lifecycle + idempotency.
  - Default-disabled state (`IsEnabled() == false` after Initialize).
  - `SetEnabled` / `IsEnabled` round-trip.
  - `SetOptions` / `GetOptions` round-trip for all bool + color +
    float fields.
  - `SetSelectedAgent` / `GetSelectedAgent` round-trip.
  - `Console_GetStatus` returns non-empty string.
  - `Update` when disabled is safe (no-op, no crash).
  - `Update` when enabled with no agents/navmesh/paths is safe.

- `Tests/TestDirtyRegionGridPhaseDD.cpp` (new, 10 tests) covering:
  - Default-constructed grid is uninitialised (MarkDirty is a no-op).
  - Initialize marks the grid as "fully dirty" (`IsFullyDirty()`
    true). Note: the `m_fullyDirty` flag is a separate signal from
    the cell bitset — `GetDirtyRects()` returns empty because
    Initialize clears both current + previous bitsets. `EndFrame`
    clears the fully-dirty flag.
  - `BeginFrame` swaps dirty buffers.
  - `MarkDirty` flags cells overlapping the rect.
  - Empty `MarkDirty` input is a no-op.
  - `MarkWidgetDirty` flags both old and new positions.
  - `MarkFullyDirty` produces non-empty merged rects.
  - `Resize` invalidates the grid to full-dirty.
  - `DirtyRect::IsEmpty` reports zero-size rects correctly.
  - Out-of-bounds rect coordinates are handled safely (clamped).

- `Tests/CMakeLists.txt` — added both new test files to the main
  source list.

## Full suite

**4769 passed, 0 failed, 1 warned, 4770 total** on
`linux-gcc-release` — +19 from Phase DD, +166 cumulative since
Phase U baseline (4605 → 4769 net Linux-active gain; Phase BB
session went to 4729, Phase CC to 4751, Phase DD to 4769, except
the flaky warn floats around).

## Playbook notes

1. **Read the real impl before asserting on complex invariants.**
   The first version of the `InitializeMarksFullyDirty` test
   expected `GetDirtyRects()` to return non-empty after Initialize —
   a reasonable inference from the header comment "Force full
   redraw on first frame". But the actual implementation sets
   `m_fullyDirty = true` as a **separate flag**, not by populating
   `m_currentDirty`. The test was wrong; `IsFullyDirty()` is the
   correct query. Phase CC caught a similar trap
   (percentages-vs-fractions). Don't trust header comments — read
   the method body.

2. **Per-instance utilities don't need lifecycle wire-up.**
   `DirtyRegionGrid` is designed for per-UI-system instantiation.
   Adding a global singleton `Initialize` / `Shutdown` to
   `GameplayLifecycleShared.cpp` would be speculative. The correct
   "activation" for per-instance utilities is **real-class test
   coverage** so any future adopter finds a locked-down contract
   (same pattern as Phase X for `HandlePool` / `TransientBufferAllocator`).

3. **Fake-coverage tests are a consistent anti-pattern.**
   `TestAIDebugRenderer.cpp` is the fifth fake-coverage file the
   session has surfaced (`TestRHIHandlePool.cpp`,
   `TestTransientBufferAllocator.cpp`, `TestNullRHIDevice.cpp`,
   `TestScriptHookManager.cpp`, now `TestAIDebugRenderer.cpp`). The
   pattern: an anonymous namespace with a class that mirrors the
   real API's shape, tested extensively, but never touching the
   production class. `-Wunused-variable` doesn't flag it because
   the reimpl IS used (by the fake tests). Grep is the only
   reliable detection: `grep '^#include.*Real/Path/FooBar.h'
   Tests/TestFooBar.cpp` — if the real header isn't included, the
   test is fake coverage.

4. **`-Wtype-limits` catches meaningless range asserts.** Initial
   test code contained `EXPECT_TRUE(rects.size() >= static_cast<size_t>(0))`
   as a "the call didn't crash" probe — a warning flagged it as
   always-true. Replaced with a side-effect assertion
   (`EXPECT_TRUE(grid.IsInitialized())`) that achieves the same
   intent without tripping `-Wtype-limits`.

## Cross-references

- Phase CC: [engine-next-steps-phase-cc-2026-04-11.md](engine-next-steps-phase-cc-2026-04-11.md)
- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
