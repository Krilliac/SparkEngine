# Engine next-steps — Phase BB (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Wire two SparkEngine singleton orphans into
`GameplayLifecycleShared.cpp` and ship real-class tests for both.
These were found by a broader orphan sweep after the Phase U/V/W/X/Y/Z/AA
themes were complete.

---

## How these were found

The Phase U+ plan's theme list ended at 3C (editor panels). After
Phase AA completed the editor-panel work, I ran a fresh orphan sweep
against SparkEngine's singletons:

```bash
for f in $(grep -rln "static.*GetInstance()" SparkEngine/Source --include='*.h'); do
  # extract class name, count references in GameplayLifecycleShared.cpp
  ...
done
```

Of the ~143 singletons in SparkEngine, most were already wired. Two
had zero external references outside their own `.cpp` / `.h` files:

1. **`Spark::Scripting::ScriptHookManager`** —
   `Engine/Scripting/ScriptHookManager.h` (251 LOC) +
   `.cpp` (176 LOC). TrinityCore-inspired gameplay hook dispatcher for
   scripts. Full implementation including `RegisterHook`,
   `UnregisterHook`, `UnregisterAllForScript`, `DispatchHook`,
   `DispatchCombatHook`, `DispatchEntityHook`, `Clear`, `HasHandlers`,
   `GetTotalHandlerCount`. Only reference in engine: one
   `@see ScriptHookManager.h` doc-comment in `DebugHookManager.h`.

2. **`Spark::Graphics::DynamicQualityScaler`** —
   `Graphics/DynamicQualityScaler.h` (112 LOC) +
   `.cpp`. Sliding-window FPS tracker that adjusts render resolution
   scale toward a target frame rate. Full implementation including
   `Initialize`, `RecordFrameTime`, `GetCurrentResolutionScale`,
   `GetRecommendedQuality`, `GetAverageFPS`, `GetAverageFrameTime`,
   `Reset`, `HasValidData`, `GetThresholds`, `GetRenderDimensions`.
   No external references at all.

Both also had **fake-coverage test files** (`TestScriptHookManager.cpp`
uses a local reimplementation of `HookType`/`HookContext`; no
`TestDynamicQualityScaler.cpp` exists at all) — the same anti-pattern
Phase X caught for the RHI orphans.

## Phase BB wire-up

### `ScriptHookManager`

Added a touch at the RenderingAndUtility block of
`GameplayLifecycleShared.cpp`:

```cpp
(void)Spark::Scripting::ScriptHookManager::GetInstance();
```

This guarantees the singleton is constructed before any script module
loads and tries to call `RegisterHook`. Teardown mirror in the
shutdown block:

```cpp
Spark::Scripting::ScriptHookManager::GetInstance().Clear();
```

which removes all registered hooks cleanly. The engine doesn't
Dispatch any hooks itself — dispatching is the responsibility of
specific gameplay systems when a real integration lands (damage
processing, entity spawn, ability cast, etc.).

### `DynamicQualityScaler`

Added an explicit `Initialize` with 60 FPS defaults at engine
startup:

```cpp
Spark::Graphics::DynamicQualityThresholds dqsDefaults;
dqsDefaults.targetFPS = 60.0f;
dqsDefaults.headroomPercent = 10.0f;
dqsDefaults.dropThresholdPercent = 5.0f;
dqsDefaults.minScale = 0.5f;
dqsDefaults.maxScale = 1.0f;
Spark::Graphics::DynamicQualityScaler::GetInstance().Initialize(dqsDefaults);
```

and a `Reset()` at shutdown. `RecordFrameTime` is **not** pumped
from the engine lifecycle — that's the render loop's responsibility
once a specific dynamic-resolution path (DLSS/FSR/TAAU/native) is
wired to consult `GetCurrentResolutionScale()`. Phase BB lands the
singleton in a constructed + initialised state so the consumer
wire-up is a one-line change when it happens.

## Files touched (code)

- `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp`:
  - `#include "Engine/Scripting/ScriptHookManager.h"`
  - `#include "Graphics/DynamicQualityScaler.h"`
  - Initialize-block additions for both singletons (in the
    `RenderingAndUtility` section, matching existing touch-based
    wire-ups for `PrefabRegistry` and `GameplayExtensionRegistry`).
  - Shutdown-block additions for both singletons.

No other engine code changes — both wire-ups are pure
lifecycle-touches. The 1000+ lines of real implementation were
already there; they just had no lifecycle connection.

## Files touched (tests)

- `Tests/TestScriptHookManagerPhaseBB.cpp` (new) — 14 tests against
  the real `Spark::Scripting::ScriptHookManager` class covering:
  singleton accessor stability, RegisterHook ID generation, Dispatch
  runs handlers, dispatch-no-handlers safety, priority ordering
  (low→high), handler cancellation, handler value modification,
  UnregisterHook by ID, UnregisterAllForScript removes only named
  script's hooks, Clear removes everything, DispatchCombatHook /
  DispatchEntityHook fill context correctly, HasHandlers per-type,
  and concurrent register + dispatch thread safety.

- `Tests/TestDynamicQualityScalerPhaseBB.cpp` (new) — 13 tests
  against the real `Spark::Graphics::DynamicQualityScaler` class
  covering: singleton stability, initialize clears window,
  thresholds round-trip, RecordFrameTime populates window,
  stable-at-target keeps max scale, below-target drops scale,
  severe underperformance clamps to minScale, above-target raises
  scale (after drop), GetRenderDimensions applies current scale,
  Reset restores max scale, and GetRecommendedQuality smoke test.

- `Tests/CMakeLists.txt` — added both new test files to the main
  test source list (not ImGui-gated — these are pure engine
  headers with no editor or ImGui dependency).

## Theme progress

Theme 3C (editor panels): complete for CI-verifiable scope (see
`engine-next-steps-phase-aa-2026-04-11.md`).

Theme 3D (ad-hoc SparkEngine singleton orphans): **Phase BB lands
the two singletons found by the broader sweep.** There may be more
orphans in other dimensions (e.g., ECS components, AI behaviors,
gameplay subsystems) — a future session can run a similar sweep
with different heuristics.

## Playbook notes

1. **Broader sweeps find what narrow ones miss.** Phase AA's panel
   glob was `Panels/*Panel.h` and missed two orphans in sibling
   directories. Phase BB's singleton glob against all of
   `SparkEngine/Source` found two more orphans in
   `Scripting/` and `Graphics/`. Mixing narrow and broad sweeps
   across sessions is the right pattern.

2. **Touch-based wire-up is the lightest possible activation.** For
   singletons that don't need explicit Initialize (lazy
   construction), a `(void)Foo::GetInstance();` at engine startup
   is enough to guarantee the singleton exists before anyone else
   reaches for it. Pair with a `Clear()` / `Shutdown()` at engine
   teardown for predictable ordering. This is the pattern Phase BB
   uses for `ScriptHookManager`.

3. **Explicit Initialize when the class owns configurable state.**
   `DynamicQualityScaler::Initialize(thresholds)` takes a
   `DynamicQualityThresholds` struct — the class needs default
   target-FPS and scale bounds to do anything useful. Phase BB
   passes sensible 60 FPS / 0.5-1.0 scale defaults in the engine
   lifecycle; downstream code can re-Initialize with game-specific
   thresholds (30 FPS for mobile, 120 FPS for esports, etc.).

4. **Don't wire the per-frame pump without a real consumer.**
   `DynamicQualityScaler::RecordFrameTime(dt)` is the per-frame
   feed, but the current render loop has no code that reads
   `GetCurrentResolutionScale` and applies it to the framebuffer.
   Pumping the scaler now would be speculative work — the scale
   would drop under load but nothing would use it. Phase BB lands
   the singleton in a clean state; a future phase that adds the
   dynamic-resolution render path wires the pump at the same time.

## Cross-references

- Phase U+ plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Previous phase: [engine-next-steps-phase-aa-2026-04-11.md](engine-next-steps-phase-aa-2026-04-11.md) (Theme 3C)
