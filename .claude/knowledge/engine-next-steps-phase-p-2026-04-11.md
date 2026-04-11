# Engine next-steps — Phase P (2026-04-11)

**Status:** Active. Phase P activates `PersistentMaterialCBManager`
— a Tier 2 graphics orphan — by making it a per-instance member of
`Spark::MaterialSystem` with `Initialize` / `Shutdown` / `BeginFrame`
lifecycle hooks on the material system's shared implementation
path.

## Context

Phases I → O wired thirteen Tier 2 graphics orphans across
`PostProcessingPipeline`, `LODManager`, `SceneRenderer`,
`LightingSystem`, and `Shader`. Phase P moves to the next obvious
lighting/material surface: `MaterialSystem` — the central material
storage + binding entry point — as the natural home for the SRP
Batcher-style persistent constant buffer manager.

`Graphics/PersistentMaterialCB.h` (220 lines) is a pure CPU manager
that keeps one large shadow buffer sub-divided into per-material
slots, with FNV-1a hash-based change detection so materials only
land on the dirty-upload list when their data actually changes.
No engine code instantiated it; the audit's "future material system
will instantiate one per material family" note was unsatisfied.

Key finding during scoping: `MaterialSystem::Initialize` and
`Shutdown` are **shared** between the Windows and Linux code paths
(unlike `LightingSystem` / `Shader` which duplicate the method
bodies). The entire implementation sits in one translation unit
with only small `#ifdef` guards around the D3D11-specific logging
lines. This means a single hook site in each of Initialize,
Shutdown, and BeginFrame is enough to wire both platforms — no
Linux stub to mirror.

## Items closed

### P1 — `PersistentMaterialCBManager` activated as a per-`MaterialSystem` member

**Problem:** `PersistentMaterialCBManager` had no engine caller. The
class implements the full SRP Batcher bookkeeping (mega-buffer
allocation, per-material slot assignment, CPU shadow, hash-based
dirty tracking, dirty slot enumeration, frame-counter stamping,
console status) but nothing in the engine's `MaterialSystem` layer
instantiated it.

**Fix:**

- `SparkEngine/Source/Graphics/MaterialSystem.h`
  - `#include "PersistentMaterialCB.h"` outside the Windows guard
    (pure CPU, no D3D11 dependency).
  - New private member
    `Spark::Graphics::PersistentMaterialCBManager m_persistentCB;`
    alongside the existing material storage. `MaterialSystem` lives
    in the global `Spark` namespace, so the member and accessor
    signatures need explicit `Spark::Graphics::` qualification —
    same pattern Phase M and Phase O used for `LightingSystem` and
    `Shader` respectively.
  - New public accessor pair `GetPersistentMaterialCB()`
    const/non-const. Downstream callers (material bind, shader
    compilation, draw command emission) can now
    `RegisterMaterial(id)`, `UpdateMaterial(id, data, size)`, and
    drain `GetDirtySlots()` through the accessor without touching
    the underlying D3D11 state.

- `SparkEngine/Source/Graphics/MaterialSystem.cpp`
  - `MaterialSystem::Initialize` calls
    `m_persistentCB.Initialize(/*maxMaterials*/ 4096, /*cbSizePerMat*/ 256)`
    after `CreateDefaultMaterials()`. Sized for 4096 materials ×
    256 bytes = 1 MB shadow buffer, matching the reference working
    set the header documentation describes.
  - `MaterialSystem::Shutdown` calls `m_persistentCB.Shutdown()`
    alongside the other material storage clears.
  - `MaterialSystem::BeginFrame` calls `m_persistentCB.BeginFrame()`
    so the manager's internal frame counter advances every render
    frame. This makes `MaterialCBSlot::lastUpdatedFrame` stamps
    reflect the real frame index, not a monotonic counter that
    never resets.

Every `MaterialSystem` instance now has a live persistent CB
manager on both the Windows and Linux implementation branches. A
future GPU-side companion that actually creates the mega
`ID3D11Buffer` can read the shadow via `GetMaterialData(id)` and
upload only the dirty sub-ranges on demand.

### Test coverage

**P-new: `Tests/TestPersistentMaterialCB.cpp`** (~260 lines, 19 tests)

Exercises the real
`Spark::Graphics::PersistentMaterialCBManager` directly. No GPU,
no D3D11 — pure CPU bookkeeping. Every test runs on every
platform's CI:

- **Lifecycle:** `InitializeAndShutdown`,
  `DefaultInitializeValues` (defaults: 4096 × 256 = 1 MB),
  `CBSizeAlignsTo16Bytes` (200 → 208 after 16-byte rounding).
- **Registration:** `RegisterBeforeInitializeFails`,
  `RegisterMaterialAllocatesContiguousSlots` (offsets 0 / 256 /
  512 for three materials),
  `RegisterDuplicateReturnsExistingOffset` (idempotent re-
  registration), `RegisterMaterialOverflowReturnsInvalid` (the
  3rd material in a 2-slot buffer returns UINT32_MAX),
  `GetBufferOffsetUnknownIsInvalid`.
- **Update + hash detection:** `UpdateUnknownMaterialFails`,
  `FirstUpdateMarksDirty`, `UpdateWithSameDataIsNoop` (hash match
  → dirty count stays zero), `UpdateWithChangedDataMarksDirty`
  (hash divergence → dirty), `UpdateSmallerThanSlotIsSafe` (32
  bytes into a 256-byte slot),
  `GetMaterialDataReturnsShadowPointer` (round-trip a
  `FakeMaterialData` with explicit RGB checks),
  `GetMaterialDataUnknownIsNull`.
- **Dirty list + frame counter:**
  `GetDirtySlotsReturnsOnlyDirty` (3 registrations → 3 dirty
  slots; `ClearDirtyFlags` drains to zero),
  `ClearDirtyFlagsResetsDirtyCount`,
  `BeginFrameAdvancesFrameStamp` (two-frame update pinning the
  strictly-increasing `lastUpdatedFrame`).
- **Console status:** `Console_GetStatusIncludesCounts` (status
  string includes material count + "dirty" label).

### Registration

`Tests/CMakeLists.txt` — `TestPersistentMaterialCB.cpp` registered
alongside the Phase O test file.

## Files touched

```
SparkEngine/Source/Graphics/MaterialSystem.h               (P1 — include + member + accessor)
SparkEngine/Source/Graphics/MaterialSystem.cpp             (P1 — Initialize / Shutdown / BeginFrame hooks)
Tests/TestPersistentMaterialCB.cpp                         (P1, new — 19 portable tests)
Tests/CMakeLists.txt                                       (P1)
.claude/knowledge/engine-next-steps-phase-p-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4523 passed, 0 failed, 1 warned**
  (pre-existing `LoadTest_Severe_EntityFlood` known-flaky), total
  119543 assertions (+48 from Phase O's baseline of 119495; the
  +19-test delta is the new `PersistentMaterialCB_*` file).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase Q

Phase P closes one more Tier 2 orphan, bringing the running total
across I→P to **fourteen orphans activated**. Remaining candidates,
in roughly ascending order of integration scope:

- **`DenoiserInterface.h`** (251 lines) — abstract denoiser plugin
  interface for RT AO / GI / reflections. The interface itself is
  ready to instantiate; the blocker is a concrete denoiser
  implementation to register as the default. Scope could be
  limited to registering a null / passthrough denoiser factory
  inside `GraphicsEngine` so downstream code can hit the
  interface path without any real denoising happening.
- **`UICompositor.h`** (231 lines) — may be superseded by
  `Engine/UI`. Needs a wire-or-delete audit: grep for existing
  UI path consumers, compare APIs, decide whether to wire the
  compositor as a UI-side layer or delete it outright.
- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise. The
  existing terrain / foliage systems have their own noise
  generators. Activation would make it an optional alternative
  backend, not a replacement — requires deciding *where* it
  becomes the default.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope, parked until real-time GI becomes a
  project priority.

**Tightest fit for Phase Q:** `DenoiserInterface` with a null
passthrough denoiser factory inside `GraphicsEngine`. Even without a
real denoiser implementation, the activation surface is a factory
registration + lifecycle tick + tests covering the interface
contract. That matches the "lifecycle-only activation" pattern
Phase N used for `ConstantBufferRing`.

## Design notes for follow-ups

- **`MaterialSystem` is the cleanest parent surface seen so far.**
  Unlike `LightingSystem` and `Shader`, `MaterialSystem` does not
  duplicate its `Initialize` / `Shutdown` bodies between Windows
  and Linux — both platforms share one implementation. Phase P
  took advantage of that by wiring the hooks once instead of
  mirroring them. Future orphan activations on material-adjacent
  subsystems should prefer this unified pattern if they get the
  choice.
- **Frame-counter activation is load-bearing.** The manager's
  `lastUpdatedFrame` field only becomes meaningful when something
  calls `BeginFrame()` on it every render frame. Wiring only
  `Initialize` / `Shutdown` would leave the manager alive but
  frame-stamped at zero forever. Phase P's
  `MaterialSystem::BeginFrame` hook is small (one line) but
  essential. New orphans with per-frame counters need the
  equivalent hook at the parent subsystem's frame boundary.
- **Hash-based dirty tracking needs zero wrapper logic.** The
  `UpdateMaterial` contract is "caller writes new data, the
  manager decides whether it's dirty" — no manual `SetDirty` call,
  no explicit invalidation. This avoids the entire class of bugs
  where a caller forgets to mark its own update dirty. Any future
  CB/UBO manager should preserve this "caller just updates, the
  manager detects the change" pattern.
- **1 MB is a reasonable default working set.** 4096 materials ×
  256 bytes per material = 1 MB matches the SRP Batcher reference
  implementations. Larger projects can override the defaults via
  `Initialize(maxMaterials, cbSizePerMat)`, but the default should
  cover typical scene working sets without needing manual
  configuration. Phase P uses the defaults at the
  `MaterialSystem::Initialize` call site.
- **The "existing coverage" audit pattern is now reliable.**
  Phases L, O, and P all found that existing orphan coverage
  claims in the audit were misleading — the real classes had
  minimal or no direct tests, with coverage spread across local
  reimplementations or smoke-test-only constant pins. The
  "Phase N–O–P design notes" pattern of "spot-check whether the
  *real symbols* are called, not just whether the class name
  appears" has now caught this three times in a row. Future
  phases should continue to assume audit coverage claims are
  optimistic until verified.
