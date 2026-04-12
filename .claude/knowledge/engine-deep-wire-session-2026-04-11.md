# Engine deep-wire session — Phases EE→II (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Consolidated report for the second half of the 2026-04-11
wire-up session. Phases EE, FF, GG, HH, II landed **13 orphans
wired or tested** across 5 commits using **parallel sweep agents**
and multiple orthogonal heuristics.

---

## Methodology: parallel sweep agents

This session dispatched two parallel `Explore` subagents to run
independent orphan-discovery sweeps across SparkEngine with
different heuristics:

1. **Agent A — Singleton & lifecycle sweep**: scan every class
   with `static.*GetInstance()` and check for zero external call
   sites, scan `Initialize()/Update()/Shutdown()` lifecycle
   classes, and cross-reference fake-coverage test files.
   Reported 12 candidates prioritised by impact-to-risk.

2. **Agent B — Utility & callback-registry sweep**: scan non-
   singleton utility classes, callback-registry patterns,
   orphan `.cpp` files, and subfolder-specific systems
   (Audio/Animation/AI/Networking). Reported 9 candidates.

Both agents ran concurrently. While they worked, I ran my own
direct grep against the 387-file test suite and found **141
fake-coverage test files** — test files that define a local
reimplementation in an anonymous namespace and never touch the
real production class. This is a separate orphan signal: even
if a class is wired, its tests may be fake.

The agents + my grep converged on the same high-priority
candidates, and together gave me ~20 leads to work through.
The per-phase execution was then ~4 orphans per batch to keep
commit sizes reasonable.

## Phase-by-phase breakdown

### Phase EE — `1263edb` (25 tests)

- **EventResponseSystem** (279+869 LOC): TrinityCore-inspired
  When/If/Then gameplay rule engine. Full lifecycle wire-up
  (Initialize/Shutdown) in `GameplayLifecycleShared.cpp`. Replaces
  the fake-coverage `TestEventResponseSystem.cpp`.
- **EntityPresetManager** (108+239 LOC): ECS preset registry for
  no-code entity spawning. Initialize-only wire-up.
- **AssetMigrationRegistry** (476 LOC header-only): asset-schema
  version registry with migration-step chaining. Full lifecycle
  wire-up.

### Phase FF — `3721c2b` (17 tests)

- **TelemetrySystem** (549 LOC header-only): analytics event
  recorder with consent management. Initialized with
  `enabled=false` + `consentGiven=false` by default (privacy-
  first); games opt in by re-Initializing with real config.
- **CacheDebugger** (403 LOC header-only): generic named-cache
  hit/miss/eviction tracker. Lazy singleton — touch-based
  wire-up + `Reset()` at shutdown.

### Phase GG — `e0beeb2` (23 new, 24 total — 1 flaky)

- **MathUtilsExtended** (276 LOC): static utility class.
  `InitializeRandom()` seeds the RNG at engine startup.
- **CpuDebugger** (455 LOC header-only): section-level CPU
  profiler. Touch-based wire-up.
- **LODGenerator** (169 LOC): QEM mesh simplification. Touch-
  based wire-up + real-class tests with a unit-cube mesh.
- **TextureCompressor** (152 LOC): BC1/BC3/BC4/BC5/BC7/ASTC
  block compression. Touch-based wire-up + tests for
  CalculateMipLevels, GetBlockSize, EstimateCompressedSize, and
  4x4 RGBA compression round-trip.

### Phase HH — `4c4f39e` (16 tests)

- **DatablockRegistry** (408 LOC header-only): Torque3D-inspired
  immutable shared-data table. Uses `Get()` not `GetInstance()`
  (the earlier sweeps missed it because of the naming
  convention). Full lifecycle wire-up (touch + Clear on shutdown).
- **NetworkSecurity** (233 LOC header-only): XOR packet
  encryption + time-limited connection tokens. Per-instance,
  real-class tests only.

### Phase II — `e23ed6f` (16 tests)

- **AIDirector** (220 LOC header-only): Left 4 Dead-inspired
  dynamic difficulty. Per-instance, real-class tests only
  (configuration + query API without touching an ECS World).
- **HLODBuilder** (473 LOC inside HLODSystem.h): build-time
  spatial-grid HLOD cluster generator. Per-instance, real-class
  tests with synthetic entity input.

## Session cumulative stats

| | Phase U start | Phase DD end | Phase EE | Phase FF | Phase GG | Phase HH | Phase II end |
|---|---|---|---|---|---|---|---|
| Tests | 4605 | 4770 | 4795 | 4811 | 4835 | 4851 | **4867** |
| New tests | 0 | +165 | +25 | +17 | +24 | +16 | +16 |
| Commits | 0 | 9 | 10 | 11 | 12 | 13 | **14** |
| Orphans wired | 0 | 15 | 18 | 20 | 24 | 26 | **28** |

**Full session totals since Phase U (2026-04-11):**
- 14 commits, 14 phases (U/V/W/X/Y/Z/AA/BB/CC/DD/EE/FF/GG/HH/II)
- 262 new tests added (4605 → 4867)
- 28 orphan classes wired into `GameplayLifecycleShared.cpp` or
  covered by real-class tests

## Orphans wired this session (cumulative Phase U → II)

| Phase | Class | Wire-up | Tests |
|---|---|---|---|
| U | `ShaderHotReload` | singleton lifecycle | 9 |
| V | `ShaderDiskCache` | singleton lifecycle + Shader::LoadShaderFromSource | 16 |
| W | `ShaderCrossCompiler` | singleton touch | 19 |
| X | `HandlePool<T,Tag>` | real-class tests | 15 |
| X | `TransientBufferAllocator` | real-class tests | 14 |
| Y | `NullRHIDevice` (rewrite) | real stub resources + HandlePool + TransientBufferAllocator | 20 |
| Z | `TransientBufferAllocator` in D3D11/D3D12/Vulkan/OpenGL | per-backend member + lifecycle hooks | (Windows CI verification) |
| AA | `LevelStreamingSystem` | editor panel registration | 11 (ImGui-gated) |
| AA | `VersionControlSystem` | editor panel registration | 11 (ImGui-gated) |
| BB | `ScriptHookManager` | singleton touch | 14 |
| BB | `DynamicQualityScaler` | singleton touch + Initialize | 11 |
| CC | `GPUStallProfiler` | singleton lifecycle | 10 |
| CC | `AsyncComputeScheduler` | singleton lifecycle | 12 |
| DD | `AIDebugRenderer` | singleton lifecycle | 9 |
| DD | `DirtyRegionGrid` | real-class tests (per-UI utility) | 10 |
| EE | `EventResponseSystem` | singleton lifecycle | 8 |
| EE | `EntityPresetManager` | singleton touch + Initialize | 7 |
| EE | `AssetMigrationRegistry` | singleton lifecycle | 10 |
| FF | `TelemetrySystem` | singleton lifecycle (disabled by default) | 7 |
| FF | `CacheDebugger` | singleton touch + Reset | 9 |
| GG | `MathUtilsExtended` | `InitializeRandom()` at startup | 5 |
| GG | `CpuDebugger` | singleton touch | 8 |
| GG | `LODGenerator` | singleton touch | 4 |
| GG | `TextureCompressor` | singleton touch | 6 |
| HH | `DatablockRegistry` | singleton touch + Clear | 8 |
| HH | `NetworkSecurity` | real-class tests (per-instance) | 8 |
| II | `AIDirector` | real-class tests (per-instance) | 8 |
| II | `HLODBuilder` | real-class tests (per-instance) | 8 |

## Fake-coverage test files surfaced

Of the ~141 fake-coverage test files in the suite, the following
were **replaced or supplemented** this session:

- `TestRHIHandlePool.cpp` → `TestRHIHandlePoolPhaseX.cpp` (real class)
- `TestTransientBufferAllocator.cpp` → `TestTransientBufferAllocatorPhaseX.cpp`
- `TestNullRHIDevice.cpp` → `TestNullRHIDevicePhaseY.cpp`
- `TestScriptHookManager.cpp` → `TestScriptHookManagerPhaseBB.cpp`
- `TestAIDebugRenderer.cpp` → `TestAIDebugRendererPhaseDD.cpp`
- `TestEventResponseSystem.cpp` → `TestEventResponseSystemPhaseEE.cpp`

That's 6 fake-coverage tests replaced. **~135 fake-coverage files
remain** — a target for future sessions.

## Playbook evolution

1. **Parallel sweep agents are high-signal.** Running two
   independent orphan-discovery sweeps with different heuristics
   converged on ~20 candidates, most confirmed by both. The
   subagent approach also kept the orphan list out of the main
   context window until the session was ready to act on it.

2. **Multiple heuristics find different orphans.**
   - `static.*GetInstance()` → most classical singletons.
   - `static.*& Get()` → Torque3D-style `Get()` pattern (Datablock-
     Registry). The original sweep missed these until I added the
     second pattern.
   - `void Initialize(` + "never referenced externally" → classes
     like MathUtilsExtended (static class with `InitializeRandom()`)
     that don't use any singleton pattern at all.
   - Fake-coverage detection (test file without a real-header
     include) → separately from orphan status, flags systems that
     are wired but untested.

3. **Per-instance orphans are still orphans.** Classes like
   `AIDirector`, `HLODBuilder`, `NetworkSecurity`, `DirtyRegionGrid`
   have no singleton, no lifecycle, and are expected to be owned
   by game modules or subsystems. The right activation for these
   is **real-class tests** that lock down the public contract so
   any future adopter finds it working. This is the same pattern
   Phase X established for `HandlePool` / `TransientBufferAllocator`.

4. **Touch-based wire-up is safe for lazy singletons.** Classes
   with no explicit Initialize (like `CacheDebugger`, `CpuDebugger`,
   `LODGenerator`, `TextureCompressor`) are lazy-initialised on
   first `GetInstance()`. A `(void)GetInstance()` touch at engine
   startup guarantees the singleton exists before any caller
   reaches for it, without forcing a speculative Initialize().

5. **Read the impl before asserting on complex behaviour.** Two
   test failures in this session were caused by my assumptions
   not matching the real impl:
   - `DirtyRegionGrid::Initialize` sets `m_fullyDirty = true` as
     a flag but doesn't populate the cell bitset — so
     `GetDirtyRects()` returns empty after Initialize. The fix
     was to test `IsFullyDirty()` instead.
   - `CacheDebugger::GetCacheStats` returns `CacheStats` by value
     (not `const CacheStats*`), with `cacheName.empty()` as the
     "not found" signal. The fix was to use value semantics + an
     empty-string check.

## Next session candidates

Remaining orphans from the sweep agents that I didn't wire this
session due to risk (platform-gated or large refactor):

- `GPUParticleSystem`, `GPUDrivenRenderer`, `GPUSkinning`,
  `GPUClusterCulling`, `MeshShaderPipeline` — all D3D11/D3D12-only
  and require render-loop integration beyond a simple lifecycle
  touch.
- `NetworkManager` (`Spark::Net`) — 1454 LOC, platform-specific
  sockets, already has a stub version wired through the
  service locator.
- `AnimationSystem` — may duplicate ECS layer animation; needs
  architecture review before wiring.
- `PacketValidator`, `QueuedEventBus` — per-instance Network/Event
  utilities ready for real-class tests.
- Remaining ~135 fake-coverage test files — rolling replacement
  as each underlying class is wired.

## Cross-references

- Phase U plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Phase DD (prior session boundary): [engine-next-steps-phase-dd-2026-04-11.md](engine-next-steps-phase-dd-2026-04-11.md)
- Commits: `1263edb` (EE), `3721c2b` (FF), `e0beeb2` (GG),
  `4c4f39e` (HH), `e23ed6f` (II).
