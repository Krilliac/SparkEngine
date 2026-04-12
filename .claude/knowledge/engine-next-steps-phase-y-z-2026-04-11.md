# Engine next-steps — Phases Y + Z (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Full production wire-up of two Theme 3B RHI orphans —
`Spark::RHI::HandlePool<T, Tag>` and `Spark::RHI::TransientBufferAllocator` —
into the five compilable RHI backends. **Second and third** phases of
Theme 3B from the Phase U+ plan.

---

## Why two phases land together

Phase Y and Phase Z share the same activation theme. Phase X landed the
real-class test coverage; Phases Y and Z land the production wire-ups.
Splitting them across two commits would not help a future reviewer
because the same orphan classes are involved, so this entry documents
both.

## Phase Y — `NullRHIDevice` rewrite

### Why this backend first?

The headless `NullRHIDevice` is the only RHI backend that compiles on
the primary `linux-gcc-release` CI job (D3D11/D3D12 are Windows-only,
Vulkan is gated behind `SPARK_VULKAN_SUPPORT` which is OFF because the
Vulkan SDK is missing, OpenGL is gated behind `SPARK_OPENGL_SUPPORT`
which is OFF because libGL is missing). Any wire-up that needs to be
validated on Linux CI **must** go through NullRHIDevice.

### Problem: NullRHIDevice returned `nullptr` from `CreateBuffer`

Pre-Phase-Y, `NullRHIDevice::CreateBuffer` was an inline one-liner:

```cpp
std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc&) override
{
    m_stats.buffersCreated++;
    return nullptr;
}
```

This broke any consumer that actually wanted to use the returned
buffer. It also made it impossible to wire `TransientBufferAllocator`
into the headless backend — the allocator needs CreateBuffer to return
a real `IRHIBuffer*` whose `MapBuffer` yields a writable pointer.

### Solution: ship concrete stub resources

New file `Graphics/RHI/NullRHIResources.h` ships five concrete stub
classes that implement the `IRHI*` interfaces:

- `NullBuffer : IRHIBuffer` — owns a `std::vector<uint8_t>` backed by
  CPU memory. `MapBuffer` on this buffer returns the vector's
  `.data()` pointer. Size, stride, and debug name round-trip through
  the descriptor.
- `NullTexture : IRHITexture` — interface-only, no backing memory.
- `NullShader : IRHIShader` — interface-only, retains stage / entry
  point / bytecode pointer from the descriptor.
- `NullSampler : IRHISampler` — interface-only.
- `NullPipelineState : IRHIPipelineState` — interface-only.

### HandlePool wire-up

`NullRHIDevice.h` now owns five `HandlePool<IRHI*, Tag, 256>` members,
one per resource type. Each `Create*` method:

1. Increments the pre-existing `m_stats` counter.
2. Constructs the corresponding `Null*` stub via `make_unique`.
3. Registers the raw pointer in the matching pool via `Allocate`.
4. Returns the unique_ptr to the caller.

`Shutdown` calls `Clear` on every pool. The pool is used as a
validation-time resource tracker, not as a lifetime manager — callers
still own the unique_ptrs.

### TransientBufferAllocator wire-up

`NullRHIDevice.h` now owns a `TransientBufferAllocator m_transientBuffers`
member initialised with modest 64 KB vertex + 32 KB index budgets (the
backend is headless so memory footprint matters).

Lifecycle:
- `Initialize`: after capabilities are finalised, call
  `m_transientBuffers.Initialize(this)`. This calls back into
  `this->CreateBuffer` (twice — vertex + index), which by now returns
  real `NullBuffer` instances, so the allocator ends up with
  CPU-backed map-able buffers.
- `BeginFrame`: call `m_transientBuffers.BeginFrame(this)`. Pumps
  `MapBuffer` on both persistent buffers.
- `EndFrame`: call `m_transientBuffers.EndFrame(this)`. Unmaps.
- `Shutdown`: call `m_transientBuffers.Shutdown(this)` **before**
  clearing the pools, so the allocator's unique_ptrs release their
  entries cleanly.

### Files touched (Phase Y code)

- `SparkEngine/Source/Graphics/RHI/NullRHIResources.h` (new) — the
  five concrete stub classes.
- `SparkEngine/Source/Graphics/RHI/NullRHIDevice.h` (rewritten) —
  returns real stubs, wires both orphans, exposes `GetBufferPool() /
  GetTexturePool() / GetShaderPool() / GetSamplerPool() /
  GetPipelinePool() / GetTransientBuffers()` accessors for tests.
- `Tests/TestGraphicsInitFallback.cpp` (updated) — the existing
  `GraphicsFallback_HeadlessResourceTracking` test baked in the old
  nullptr contract; it now asserts the new non-null contract and
  baselines its "user-created" count off the post-init stats.

### Phase Y tests

`Tests/TestNullRHIDevicePhaseY.cpp` ships **20 tests** covering:
- Real stub resources returned from all 5 `Create*` methods.
- `MapBuffer` on a `NullBuffer` returns a writable CPU pointer.
- Each of the 5 `HandlePool` members grows with `Create*` calls.
- `Shutdown` clears every pool and the transient allocator.
- `TransientBufferAllocator` reports the correct 64 KB / 32 KB budgets.
- `BeginFrame` followed by `AllocateVertices / AllocateIndices`
  returns valid blocks; allocations outside a frame fail.
- Frame-to-frame offset reset.
- Over-budget allocations fail.
- Reinitialize-after-shutdown lifecycle.
- Immediate command list still accessible.

Added to `Tests/CMakeLists.txt`.

### Phase Y results

Full SparkTests suite after Phase Y: **4704 passed, 0 failed, 1 warned**
(the warn is the pre-existing flaky replication test). +22 tests vs.
Phase X baseline.

## Phase Z — `TransientBufferAllocator` in real backends

### Scope

The Phase U+ plan's Theme 3B listed four per-backend phases (X/Y/Z/AA)
for wiring handle pool + transient allocator into each real RHI
backend. Phase Z consolidates the **TransientBufferAllocator** wire-up
across all four compilable real backends in a single commit because
the per-backend edit is identical and tiny (one header include, one
member, four one-liner method calls).

**HandlePool is deliberately not wired into real backends** because:

1. Real backends already track resources via `unique_ptr` ownership
   and have their own deferred-deletion queues. Adding an unused
   handle pool would be pure speculation / future-proofing — a
   violation of the CLAUDE.md anti-bloat rules.
2. The `@note` on `RHIHandlePool.h` explicitly says backends
   instantiate handle pools **"as they are added"**, not on a
   scheduled activation. Real backends have not added a use case.
3. NullRHIDevice (Phase Y) genuinely benefits from handle pool
   tracking because it's the test backend where resource lifecycle
   introspection matters.

### Per-backend edits

Every real backend gets the same minimal wire-up:

1. `#include "../TransientBufferAllocator.h"` in the device header.
2. `TransientBufferAllocator m_transientBuffers{4 MB, 2 MB}` member
   (default budgets — real backends have real GPU memory).
3. `m_transientBuffers.Initialize(this)` in `Device::Initialize`
   after the device is ready.
4. `m_transientBuffers.BeginFrame(this)` in `Device::BeginFrame`.
5. `m_transientBuffers.EndFrame(this)` in `Device::EndFrame`.
6. `m_transientBuffers.Shutdown(this)` in `Device::Shutdown` before
   the device is torn down.

### Files touched (Phase Z code)

- `SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.h` — member +
  include.
- `SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.cpp` — Init /
  BeginFrame / EndFrame / Shutdown hooks.
- `SparkEngine/Source/Graphics/RHI/D3D12/D3D12Device.h` — member +
  include.
- `SparkEngine/Source/Graphics/RHI/D3D12/D3D12Device.cpp` — Init /
  BeginFrame / EndFrame / Shutdown hooks. Shutdown hook runs before
  `WaitForIdle` + `ProcessDeferredReleases` so the transient buffers
  enter the deferred queue cleanly.
- `SparkEngine/Source/Graphics/RHI/Vulkan/VulkanDevice.h` — member +
  include.
- `SparkEngine/Source/Graphics/RHI/Vulkan/VulkanDevice.cpp` — Init
  hook after `CreateDescriptorSetLayout`; Shutdown hook before
  `vkDeviceWaitIdle`.
- `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.h` — member +
  include (class is named `GLDevice`, not `OpenGLDevice`).
- `SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp` — Init
  hook after the context is current; Shutdown hook right after the
  `m_shutdownCalled` guard.

### Phase Z CI coverage

**None of the four real backends compile on `linux-gcc-release`:**

| Backend | Compilation gate | Linux CI status |
|---|---|---|
| D3D11Device | `#ifdef _WIN32` | **Not compiled** |
| D3D12Device | `#ifdef _WIN32` | **Not compiled** |
| VulkanDevice | `SPARK_VULKAN_SUPPORT` | **Not compiled** (Vulkan SDK missing) |
| GLDevice | `SPARK_OPENGL_SUPPORT` | **Not compiled** (libGL missing) |

The Phase Z wire-ups will be validated by the Windows CI jobs
(`build-windows-vs2022`, `build-windows-vs2026`) and any Linux job
that enables one of the gated backends. This is tracked as a
**known deferred verification** — Phase Z code is minimal (one
include, one member, four one-liner calls per backend) so the risk
of a Windows compile break is low but not zero.

Phase Y's `NullRHIDevice` rewrite is the Linux-testable reference
implementation — the same pattern applied to real backends.

## Files touched (tests)

- `Tests/TestNullRHIDevicePhaseY.cpp` (new, 20 tests) — see Phase Y
  above.
- `Tests/CMakeLists.txt` — added `TestNullRHIDevicePhaseY.cpp`.
- `Tests/TestGraphicsInitFallback.cpp` — updated
  `GraphicsFallback_HeadlessResourceTracking` test to match the new
  non-null contract and post-init baseline.

## Theme 3B scoreboard

| Orphan | Phase | Status |
|---|---|---|
| `Spark::RHI::HandlePool<T, Tag>` | X | **Real-class tests shipped** (15 tests) |
| `Spark::RHI::HandlePool<T, Tag>` | Y (NullRHIDevice) | **Wired + 20 integration tests** |
| `Spark::RHI::HandlePool<T, Tag>` | Real backends | **Deliberately deferred** — per `@note` on-demand guidance |
| `Spark::RHI::TransientBufferAllocator` | X | **Real-class tests shipped** (14 tests) |
| `Spark::RHI::TransientBufferAllocator` | Y (NullRHIDevice) | **Wired + integration tests** |
| `Spark::RHI::TransientBufferAllocator` | Z (D3D11) | **Wired — Windows CI verification** |
| `Spark::RHI::TransientBufferAllocator` | Z (D3D12) | **Wired — Windows CI verification** |
| `Spark::RHI::TransientBufferAllocator` | Z (Vulkan) | **Wired — gated CI verification** |
| `Spark::RHI::TransientBufferAllocator` | Z (OpenGL) | **Wired — gated CI verification** |

Theme 3B status: **fully wired on every compilable-on-Linux backend,
untested on gated / Windows-only backends by design**. Any future
session that enables Vulkan/OpenGL or runs Windows CI will
immediately catch a compile break if one exists.

## Playbook notes for future phases

1. **Don't force HandlePool into unique_ptr-owning backends.** Real
   RHI backends already track resources via unique_ptr + deferred
   deletion. Adding an unused HandlePool member is pure speculation.
   Wire it in only when the backend has a concrete use case — e.g.,
   bindless descriptor index allocation, command-list lifetime
   tracking, or a specific resource type that needs stale-handle
   detection.

2. **Stub out interface-only return types for test/Null backends.**
   Returning `nullptr` from a factory method in a test backend is
   worse than returning a no-op stub. Stubs let downstream consumers
   test their logic. The new `NullRHIResources.h` adds ~80 lines for
   five stubs and unlocks the entire `NullRHIDevice` wire-up plus any
   other code path that assumed `CreateBuffer` could return null.

3. **Cross-TU member ordering in `Shutdown`.** In NullRHIDevice, the
   `TransientBufferAllocator::Shutdown` call must precede the pool
   `Clear` calls because the allocator owns `unique_ptr<IRHIBuffer>`
   instances that are registered in the pool. Clearing the pool
   first would leave the allocator's destructors running against
   entries that no longer exist in the pool (harmless — the pool is
   an observer — but confusing). Prefer lifetime-order teardown.

4. **Phase Z's test coverage gap is a deliberate trade-off.** Wiring
   real backends on Linux CI is impossible until Vulkan SDK /
   libGL / Wine-DXVK are installed in the CI image. The alternatives
   were (a) write the wire-ups but leave them disabled, (b) wait for
   CI infrastructure, or (c) ship the wire-ups as-is and trust
   Windows CI. Option (c) is acceptable because the code is
   mechanical and small per backend. Future sessions that enable a
   gated backend on Linux will inherit all this wire-up for free.

5. **CLAUDE.md bloat rules saved this phase from scope creep.** The
   original temptation was to wire `HandlePool` into every backend
   for symmetry. The anti-bloat rule "**Am I future-proofing? Stop.
   Write only what is needed today.**" forced the honest call: real
   backends don't currently need handle tracking, so don't add it.

## Cross-references

- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Phase X (real-class tests): [engine-next-steps-phase-x-2026-04-11.md](engine-next-steps-phase-x-2026-04-11.md)
- Theme 3A complete: [engine-next-steps-phase-w-2026-04-11.md](engine-next-steps-phase-w-2026-04-11.md)

## Next

- Theme 3C — Editor panel activation. Per the Phase U+ plan, the
  next session should survey `SparkEditor/Source/Panels/` against
  `EditorPanelFactory` and identify any panels that are defined but
  not registered.
- Or: backfill test coverage for the real backends as CI gains
  support for Vulkan / OpenGL on Linux.
