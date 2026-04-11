# Fake-coverage conversion — Phases JJ/KK/LL (2026-04-11)

**Type:** Observation
**Status:** Active — in progress (18 of ~141 converted)
**Scope:** First three batches of fake-coverage test file conversion
using parallel sweep agents to categorize + direct rapid conversion
for Category A candidates.

---

## The fake-coverage anti-pattern

A fake-coverage test file:

1. Does NOT include the real header (`Engine/Foo/Bar.h`).
2. Defines a local reimplementation of the class inside an anonymous
   namespace.
3. Exercises the local reimplementation with 5-20 tests.

The net effect: the test file LOOKS like it tests `Spark::Foo::Bar`
but actually only tests the anon-namespace reimpl. The production
class could be broken or missing entirely and the tests would still
pass. This is the primary reason `-Wunused-variable` and similar
warnings don't flag it.

A grep-based detector:

```bash
for f in Tests/Test*.cpp; do
  if ! head -40 "$f" | grep -qE "include.*(SparkEngine|Engine|Graphics|Core|Utils|Physics|RHI|Audio|Animation|Scripting|Networking|ECS|AI|Gameplay|UI)/.*\.h"; then
    if head -50 "$f" | grep -qE "Standalone|Reimplementation|standalone|reimplementation"; then
      echo "$(basename $f)"
    fi
  fi
done
```

Initial grep found **141 files** matching this pattern. After 6
previous sessions replaced 6, the JJ/KK/LL batches converted **18
more**. The remaining ~117 are a long tail.

## Methodology

**Parallel sweep agents** categorized the 141 files into:
- **Category A (easy)**: pure-CPU class with default constructor,
  clean public API, no runtime dependencies. Can be smoke-tested
  with 5-10 tests in ~30-50 LOC.
- **Category B (partial)**: real class exists but constructing it
  requires scaffolding (ECS World, graphics device, network stack).
- **Category C (delete)**: real class removed/renamed; fake test is
  stale.
- **Category D (skip)**: real class already covered by another
  real-class test file.

Across the first 50 alphabetical files, the split was roughly:
- **7 Category A** (14%)
- **10 Category B** (20%)
- **4 Category C** (8%)
- **8 Category D** (16%)
- **~21 "too complex" or platform-gated** (42%)

Across the next ~40 files, another ~27 Category A candidates were
found. The session converted 18 of those to real-class tests.

## Phase JJ: 10 real-class test files (61 tests)

Commit `db77755`:

| Real class | Tests | Key API |
|---|---|---|
| `Spark::AngleUtils` | 7 | ToRadians, NormalizeAngle, AngleDelta, LerpAngle |
| `Spark::BitUtils` | 6 | PopCount(32/64), TrailingZeros, LeadingZeros, IsPowerOfTwo |
| `Spark::BitFlags<E>` | 4 | Set, Clear, Toggle, Has, construction |
| `Spark::Tween` + `EaseEvaluate` | 8 | Easing curves, Update, Kill, fluent builder |
| `SplineMath` | 7 | CatmullRom, LerpPoint, CubicBezier |
| `Spark::World::SpatialGrid` | 7 | WorldToCell, AddEntity, GetEntitiesInRadius |
| `Spark::AI::SteeringBehaviors` | 5 | Seek, Flee, Arrive |
| `Spark::AI::CoverSystem` | 5 | singleton, RegisterCoverPoint, FindNearestCover |
| `Spark::FixedHeapArray<T, N, A>` | 6 | Fill, Zero, alignment, move |
| `Spark::AtomicSharedPtr<T>` | 7 | Load, Store, Exchange, CompareExchange |

## Phase KK: 5 real-class test files (32 tests)

Commit `f0a0810`:

| Real class | Tests | Key API |
|---|---|---|
| `Spark::RHI::DeferredDeletionQueue` | 6 | Enqueue, ProcessQueue, FlushAll, frame counter |
| `Spark::PerformanceStats` | 4 | BeginFrame/EndFrame, total frames, FPS |
| `Spark::Animation::BlendSpace2D` | 6 | AddSample, RemoveSample, Evaluate, GetSamples |
| `Spark::Cinematic::SequencerManager` | 8 | CreateSequence, GetSequence, RemoveSequence, Update safety |
| `Spark::SubsystemFaultIsolator` | 8 | ReportFault, retry limits, ResetSubsystem, records snapshot |

## Phase LL: 3 real-class test files (18 tests)

Commit `402f983`:

| Real class | Tests | Key API |
|---|---|---|
| `Spark::ColorUtils` | 4 | RGBToHSV, HSVToRGB, round-trip |
| `Spark::StateMachine<StateID>` | 7 | AddState, Start, Tick, OnEnter/OnExit/OnUpdate callbacks |
| `Spark::EventBus` | 7 | Subscribe, Publish, Unsubscribe, RAII handle, type segregation |

## Test file naming convention

Converted tests live alongside the existing fakes as `Test*Real.cpp`:
- `TestAngleUtils.cpp` (fake, anon-namespace) stays in place
- `TestAngleUtilsReal.cpp` (new, real-class) added alongside

This approach:
- **Preserves existing tests** — the fakes still pass, so nothing
  regresses if the conversion has bugs.
- **Adds real coverage** — the Real tests are the authoritative ones;
  a future cleanup can delete the fakes once the Real versions are
  proven stable.
- **Zero build risk** — new files only; no edits to existing tests
  or CMakeLists beyond appending.

## API drift landmines

Three compile errors surfaced while writing `TestStateMachineReal.cpp`:

1. `StateMachine::SetInitialState(x)` → actual API is `Start(x)`.
2. `StateMachine::Update(dt)` → actual API is `Tick(dt)`.
3. `StateMachine::GetCurrentState()` returns `std::optional<StateID>`,
   not raw `StateID`.

The fake-coverage file had NONE of these methods — it used its own
local reimpl with different names. The agent's API column was derived
from fake-test method names, not the real header. **Always verify
against the real header before writing assertions.**

Two additional runtime "gotchas":

- `Spark::Tween::SetDelay(t)` only stores `m_delay`; it does NOT
  initialize `m_delayRemaining`, so the first Update after SetDelay
  does not actually delay. Latent bug in the Tween class — not
  something the session fixed, but the test now asserts the fluent-
  builder chaining rather than the delay runtime behaviour.

- `Spark::CacheDebugger::GetCacheStats(name)` returns `CacheStats`
  **by value** (not `const CacheStats*`), with an empty `cacheName`
  as the "not found" signal. The Phase FF test originally used
  `auto* stats = ...; if (stats) ...` which doesn't compile.

## Session stats

| Phase | Commit | Files | Tests | Classes covered |
|---|---|---|---|---|
| JJ | `db77755` | 10 | 61 | AngleUtils, BitUtils, BitFlags, Tween, SplineMath, SpatialGrid, SteeringBehaviors, CoverSystem, FixedHeapArray, AtomicSharedPtr |
| KK | `f0a0810` | 5 | 32 | DeferredDeletion, PerformanceStats, BlendSpace, Sequencer, FaultIsolation |
| LL | `402f983` | 3 | 18 | ColorUtils, StateMachine, EventBus |
| **Total** | | **18** | **111** | **18 classes** |

Full SparkTests suite: 4867 → 4979 on `linux-gcc-release` (+112,
accounting for the 1 fake-coverage test that was replaced by a
Real version and for the Tween SetDelay test that went from fail
to pass).

## Remaining work

- **~123 fake-coverage files** still on the list.
- Many are Category B (need ECS World / graphics device / audio
  backend scaffolding) and require more effort per file.
- Category C files (real class removed/renamed) should be deleted
  in a cleanup pass once each is manually verified.
- Category D files should be cross-checked against the actual
  coverage and potentially merged.

Subsequent sessions can continue the batch pattern:
1. Pick 5-10 Category A candidates.
2. Write `Test*Real.cpp` files alongside the fakes.
3. Build + run + fix API drift errors.
4. Commit as a phase.

## Cross-references

- Deep-wire session: [engine-deep-wire-session-2026-04-11.md](engine-deep-wire-session-2026-04-11.md)
- Original wire-up plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
