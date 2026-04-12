# Engine next-steps — Phase X (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Activate real-class test coverage for two Theme 3B RHI
orphans — `Spark::RHI::HandlePool<T, Tag>` and
`Spark::RHI::TransientBufferAllocator`. **First** phase of Theme 3B
(RHI backend parity) from the Phase U+ plan.

---

## Why this phase is different from U/V/W

Phases U, V, and W followed the same rhythm: find an orphan class with
zero production call sites, add a singleton accessor, wire it into
`Shader::Initialize`, write tests against the real class. Phase X
**deliberately diverges** from this pattern because both Theme 3B
target orphans have explicit `@note` headers saying they are
**intentional demand-driven utilities** and do **not** want
singleton-style lifecycle wiring:

- `Graphics/RHI/RHIHandlePool.h` `@note`:
  > "Intentional generic utility template — pure C++ with no GPU
  > dependency. There is no singleton to wire into a lifecycle; RHI
  > device implementations (D3D11, Vulkan, ...) instantiate one per
  > resource type as they are added."

- `Graphics/RHI/TransientBufferAllocator.h` `@note`:
  > "Intentional per-frame allocator utility — one instance is
  > expected to be owned by each render system that needs cheap
  > transient vertex/index memory (particles, debug draw, UI,
  > decals). No singleton, no engine lifecycle wiring."

Forcing a singleton into either class would be AI-bloat: adding a
production call site the author explicitly didn't want.

**So what does Phase X actually activate?** The playbook step 6 from
Phase U+ plan says:

> "Tests against the real class. Phases L / O / P all found that
> audit-claimed 'existing coverage' was actually against a local
> test-file reimplementation. Grep the test file for the real
> class's fully-qualified name before trusting the claim."

Both existing test files are exactly that anti-pattern:

- `Tests/TestRHIHandlePool.cpp` defines `TestHandles::TypedHandle` and
  `TestHandles::HandlePool` in an anonymous namespace and never
  touches `Spark::RHI::HandlePool`.
- `Tests/TestTransientBufferAllocator.cpp` defines a
  `MockTransientAllocator` with its own allocation math and never
  touches `Spark::RHI::TransientBufferAllocator`.

Phase X's activation **is the real-class coverage** — Phase X ships
new test files that exercise the actual headers, locking down their
public contracts so any future backend that adopts them on demand
can't silently break the API.

## Files touched (tests only — no engine code)

- `Tests/TestRHIHandlePoolPhaseX.cpp` (new) — 13 tests against the
  real `Spark::RHI::HandlePool<T, Tag>` template and
  `Spark::RHI::TypedHandle<Tag>` struct. Uses a local `FakeBuffer`
  POD as the pool's resource type so the test has no GPU dependency.

  Coverage:
  1. `TypedHandleRoundTrips` — `Make(idx, gen, tag)` bit-packing
     round-trips through `GetIndex / GetGeneration / GetTag`.
  2. `DefaultHandleInvalid` — default-constructed handle has
     `IsValid() == false` and zero fields.
  3. `HandleEqualityByValue` — `operator==` / `operator!=` compare
     raw 64-bit values.
  4. `AllocateReturnsValidHandle` — first Allocate returns a valid
     handle and bumps `Count()`.
  5. `GetReturnsAllocatedPointer` — `Get(handle)` returns the
     stored pointer; `IsValid(handle)` is true.
  6. `GetOnInvalidHandleReturnsNullptr` — default handle returns
     `nullptr` from `Get`.
  7. `FreeBumpsGenerationAndInvalidates` — after Free, the original
     handle is stale (`Get` returns nullptr); a subsequent Allocate
     into the same slot has a strictly greater generation.
  8. `FreeOnInvalidHandleReturnsFalse` — Free on default handle
     returns false.
  9. `DoubleFreeIsRejected` — Free on an already-freed handle
     returns false.
  10. `CapacityEnforced` — allocations past Capacity return
     invalid handles; freeing one makes room again.
  11. `ClearResetsPool` — Clear drops Count to zero and subsequent
     allocations succeed.
  12. `TagDiscriminatesHandleTypes` — `BufferTag` pools emit tag=1
     handles, `TextureTag` pools emit tag=2.
  13. `AllTagsProduceDistinctValues` — the five engine-wide aliases
     (`BufferHandle`, `TextureHandle`, `ShaderHandle`,
     `SamplerHandle`, `PipelineHandle`) produce tags 1–5 respectively.
  14. `CopiedHandleStillValid` — copy-constructed handles share
     the same generation and invalidate together.
  15. `MultipleAllocFreeCyclesAdvanceGenerations` — after many
     alloc/free cycles at least one slot's generation has advanced
     past 1.

- `Tests/TestTransientBufferAllocatorPhaseX.cpp` (new) — 13 tests
  against the real `Spark::RHI::TransientBufferAllocator`. Ships two
  local helper classes in an anonymous namespace:
    - `FakeBuffer : IRHIBuffer` — owns a CPU-backed `std::vector<uint8_t>`
      so `MapBuffer` can return a writable pointer.
    - `FakeDevice : IRHIDevice` — minimal stub of every pure virtual
      on `IRHIDevice`. `CreateBuffer` returns a `FakeBuffer`;
      `MapBuffer` returns its CPU pointer; `UnmapBuffer` is a
      counter-bumping no-op. Everything else returns nullptr or
      no-ops.

  Coverage:
  1. `InitializeNullptrFails` — Initialize(nullptr) returns false.
  2. `InitializeSucceedsWithFakeDevice` — creates exactly two
     buffers (vertex + index) on success; budget getters match the
     constructor parameters.
  3. `AllocateVerticesReturnsValidBlock` — Alloc returns a valid
     allocation with the right `sizeBytes` + `offsetBytes`, the
     `data` pointer is writable (memset confirms).
  4. `AllocateIndicesAdvancesIndexOffset` — index allocation bumps
     `GetIndexBytesUsed`.
  5. `SixteenByteAlignment` — consecutive unaligned-size allocations
     produce offsets that are multiples of 16.
  6. `OverBudgetAllocationFails` — past-budget Alloc returns invalid
     and does **not** advance the offset.
  7. `HasVertexSpaceReportsAvailability` — query returns true before
     and false after budget exhaustion.
  8. `HasIndexSpaceReportsAvailability` — same for index buffer.
  9. `BeginFrameMapsEndFrameUnmaps` — `FakeDevice::GetMapCalls`
     increments by 2 on BeginFrame, `GetUnmapCalls` by 2 on EndFrame.
  10. `AllocationAfterEndFrameInvalid` — after EndFrame the mapped
     pointers are null, so subsequent allocations report invalid.
  11. `BeginFrameResetsOffsets` — a second frame starts with
     vertex/index bytes-used at zero.
  12. `BudgetGettersMatchConstructor` — constructor params survive.
  13. `DefaultBudgetsArePositive` — default ctor uses 4 MB vertex +
     2 MB index.
  14. `ShutdownReleasesBuffers` — Shutdown + Re-Initialize creates
     a fresh pair of buffers (tracked by cumulative count).

- `Tests/CMakeLists.txt` — added both new test files.

## No engine code changes

Phase X touches **only test files**. The `@note` headers on both
orphan classes explicitly say they are on-demand utilities. Backends
(D3D11, Vulkan, etc.) will add instances as they grow their resource
management. Phase X does not force a premature use site.

## Theme 3B scoreboard

| Phase | Orphan | Status | Nature |
|---|---|---|---|
| X | `Spark::RHI::HandlePool<T, Tag>` | **Real-class tests** — 13 against real class | Header-only template |
| X | `Spark::RHI::TransientBufferAllocator` | **Real-class tests** — 13 against real class with fake RHI device | Header-only per-system utility |

Theme 3B orphan count (test-coverage-against-real-class): 2 → 0.
Remaining Theme 3B items from the plan (real production use sites in
`VulkanDevice`, `D3D12Device`, `OpenGLDevice`) are deferred — those
phases would refactor real backend code and are **not** minimum-viable
activations. They land when a specific backend naturally grows to
need one.

## Playbook notes for future phases

1. **Some orphans say "do not wire me".** When a header `@note`
   explicitly says the class is a demand-driven utility with no
   singleton wiring, respect it. The "activation" for these orphans
   is **real-class test coverage**, not forced singleton wires.
   Phase X is the template for this sub-pattern: new test files
   against the real symbols, zero engine code changes.

2. **Writing a minimal fake RHI device is cheap.** Phase X's
   `FakeDevice` in `TestTransientBufferAllocatorPhaseX.cpp` is
   ~80 lines and stubs every pure virtual on `IRHIDevice`. Future
   phases that need to test any class requiring an `IRHIDevice*`
   can copy this pattern verbatim — or extract it to
   `Tests/FakeRHIDevice.h` if enough sites grow to need it.

3. **Check for local reimplementations first.** Before writing new
   tests for any orphan, grep the existing Tests/ folder for the
   real class name. If the test file exists but references a
   reimplementation (anonymous namespace + duplicate struct), that
   IS the orphan signal — the tests don't actually cover the
   production code. Phase X caught two of these; Theme 3B's other
   orphans (if any) should be audited the same way.

4. **Aggregate-initialisation with designated initialisers.** Phase
   X uses `FakeBuffer buf{.id = 42};` — valid in C++20+ and matches
   the codebase's C++23 baseline. Prefer this over constructor
   proliferation for POD test helpers.

## Cross-references

- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Previous phase: [engine-next-steps-phase-w-2026-04-11.md](engine-next-steps-phase-w-2026-04-11.md) — final Theme 3A activation
- Theme 3B next steps: per-backend instantiation in `VulkanDevice` /
  `D3D12Device` / `OpenGLDevice`. Deferred until a specific backend
  naturally needs one — the `@note` headers are explicit that these
  are on-demand utilities, not scheduled activations.
