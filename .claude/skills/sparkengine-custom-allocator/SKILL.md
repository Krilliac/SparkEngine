---
name: sparkengine-custom-allocator
description: >-
  Reference for SparkEngine's four hand-rolled memory allocators — FrameAllocator (per-frame bump),
  LockFreeRingAllocator (producer/consumer ring), HandleAllocator/VersionedHandle (generational
  handles), and RHI TransientBufferAllocator (GPU vertex/index suballocation). TRIGGER when you are
  about to write per-frame or transient allocations, ask "which allocator should I use", need the exact
  method signatures / thread-safety of one of these, wonder why the code uses naked buffers instead of
  new/delete, or need to prevent use-after-free on recycled slots/indices. DO NOT TRIGGER for hunting an
  actual leak or use-after-free bug (use the memory-hunt skill), for general std::unique_ptr ownership
  questions, or for GPU resource lifetime / ComPtr / RHI backend selection.
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine custom allocators

SparkEngine follows a strict "no naked `new`/`delete`, `std::unique_ptr` owns, raw pointers are
non-owning" rule (see `D:\SparkEngine\CLAUDE.md` -> Coding Standards). These four allocators are the
**deliberate, sanctioned exceptions**. Each exists because a general-purpose `new`/`delete` or
`std::vector` would be too slow, too fragmenting, or unable to detect a specific bug class. This skill
documents their real APIs, lifetimes, thread-safety, and when to reach for each.

All four live in namespace `Spark` (the RHI one in `Spark::RHI`). All are header-only.

| Allocator | Header | Namespace | Purpose |
|-----------|--------|-----------|---------|
| `FrameAllocator` | `SparkEngine/Source/Utils/FrameAllocator.h` | `Spark` | CPU per-frame linear/bump allocation of short-lived POD |
| `LockFreeRingAllocator<N>` | `SparkEngine/Source/Utils/LockFreeRingAllocator.h` | `Spark` | FIFO producer/consumer ring for streaming data |
| `HandleAllocator` + `VersionedHandle` | `SparkEngine/Source/Utils/VersionedHandle.h` | `Spark` | Generational index handles that detect use-after-free |
| `TransientBufferAllocator` | `SparkEngine/Source/Graphics/RHI/TransientBufferAllocator.h` | `Spark::RHI` | GPU vertex/index suballocation for per-frame dynamic geometry |

> When NOT to use this skill: if you are chasing a live leak or use-after-free, use the global
> **memory-hunt** skill (leak/UAF hunting methodology) instead — this skill only describes the
> allocator APIs. For plain ownership (`unique_ptr` vs raw pointer) questions, follow CLAUDE.md's
> Coding Standards, not this document.

---

## Decision guide — which allocator do I reach for?

Answer top-to-bottom; take the first row that matches.

| If you need... | Use | Why |
|----------------|-----|-----|
| Scratch CPU memory that lives for exactly one frame and is thrown away wholesale | `FrameAllocator` | O(1) bump alloc, O(1) `Reset()`, zero per-object free cost |
| GPU-visible vertex/index memory for dynamic geometry drawn this frame (particles, debug lines, UI, decals) | `TransientBufferAllocator` (`Spark::RHI`) | Maps one Dynamic buffer per frame with DISCARD, bumps an offset — no per-object GPU buffer |
| A stable ID for an object that may be freed and its slot reused, and you must detect stale IDs | `HandleAllocator` + `VersionedHandle` | Generation bump makes an old handle fail `IsValid()` instead of aliasing a new object |
| A single-producer / single-consumer FIFO stream of variable-size records across threads | `LockFreeRingAllocator<N>` | Lock-free SPSC ring with size-prefixed records; producer allocates, consumer frees in order |
| Long-lived ownership of a single object | none of these — use `std::unique_ptr` | Custom allocators do not call destructors / do not track lifetime |

Grounded real usages in the engine (non-test):

- `FrameAllocator` — `SparkEngine/Source/Graphics/SceneRenderer.h:187` holds
  `Spark::FrameAllocator m_frameAllocator{4 * 1024 * 1024};` (4 MB) for the per-frame draw list.
- `TransientBufferAllocator` — `SparkEngine/Source/Graphics/RHI/RHIDeviceBase.h:45` embeds one per
  RHI device (`{4 * 1024 * 1024, 2 * 1024 * 1024}`), pumped by each backend's `BeginFrame`/`EndFrame`
  (e.g. `D3D11Device::BeginFrame` -> `m_transientBuffers.BeginFrame(this)`,
  `SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.cpp:1301`).
- `LockFreeRingAllocator` and `HandleAllocator`/`VersionedHandle` — as of 2026-07-07 have **no
  non-test call sites** in engine source; they are exercised only by `Tests/`. Treat them as
  available utilities (`candidate` for wider use), not yet load-bearing in a shipping path.

---

## FrameAllocator — per-frame CPU bump allocator

**What it is:** a fixed-capacity byte buffer with a monotonically increasing offset. Allocation just
advances the offset; there is no per-object free. You call `Reset()` once per frame to reclaim
everything at once.

**Why it exists instead of `new`/`delete`:** eliminates heap fragmentation and malloc/free overhead
for thousands of tiny short-lived objects (render commands, temporary arrays). Freeing is a single
integer assignment.

### API (verified against the header)

```cpp
explicit FrameAllocator(size_t capacityBytes = 1024 * 1024);   // allocates via ::operator new(nothrow)

void*  AllocRaw(size_t bytes, size_t alignment = alignof(std::max_align_t)); // nullptr if OOM
void*  Allocate(size_t size, size_t alignment = 16);           // alias for AllocRaw
template<typename T> T* Alloc(size_t count = 1);               // uninitialized storage for count T
template<typename T, typename... Args> T* New(Args&&... args); // placement-new one T

void   Reset();          // O(1): offset = 0. Does NOT run destructors.
size_t Used()  const;    size_t GetUsed()     const;   // bytes used this frame
size_t Capacity() const; size_t GetCapacity() const;
size_t Remaining() const;
size_t PeakUsed()  const;  // high-water mark
void   UpdatePeak();       // call before Reset() to record the peak
```

Move-only (copy deleted, move defined). Non-owning of what you build inside it.

### Critical rules

- **Trivially destructible only.** `Reset()` and the destructor do **not** call your objects'
  destructors. Only store PODs / trivially-destructible types, or types whose destructors are no-ops.
- **`New`/`Alloc` return `nullptr` when out of space** — always null-check. There is no growth.
- Alignment must be a power of two — enforced by `SPARK_EXPECTS` (from `Core/Contracts.h`).
- Not thread-safe. One `FrameAllocator` per thread, or serialize access externally.
- Pointers are invalidated by `Reset()`. Never hold a `FrameAllocator` pointer across frames.

### Typical lifecycle

```cpp
FrameAllocator alloc(4 * 1024 * 1024);   // once at startup / as a member
// --- each frame ---
auto* cmds = alloc.Alloc<DrawCommand>(count);   // bump
// ... fill and consume cmds this frame ...
alloc.UpdatePeak();
alloc.Reset();                                   // reclaim all, O(1)
```

---

## TransientBufferAllocator — GPU per-frame vertex/index suballocation

**What it is (`Spark::RHI`):** the GPU analogue of `FrameAllocator`. It owns one large Dynamic vertex
buffer and one Dynamic index buffer. Each frame both are mapped with DISCARD, allocations bump an
offset into the mapped pointer, and at `EndFrame` the buffers are unmapped. Inspired by bgfx's
transient buffers.

**Why it exists:** dynamic geometry (particles, debug lines, UI, decals) changes every frame. Creating
a GPU buffer per object each frame would be catastrophic; this suballocates from two persistent buffers
instead.

### API (verified against the header)

```cpp
explicit TransientBufferAllocator(uint32_t vertexBudget = 4*1024*1024,
                                  uint32_t indexBudget  = 2*1024*1024);

bool Initialize(IRHIDevice* device);   // create the two Dynamic buffers; call once at startup
void Shutdown(IRHIDevice* device);     // release buffers; called by destructor with nullptr

void BeginFrame(IRHIDevice* device);   // resets offsets AND maps both buffers — call before allocating
void EndFrame(IRHIDevice* device);     // unmaps; allocations from this frame become invalid

TransientAllocation AllocateVertices(uint32_t vertexCount, uint32_t vertexStride);
TransientAllocation AllocateIndices (uint32_t indexCount,  uint32_t indexStride = sizeof(uint32_t));

bool HasVertexSpace(uint32_t vertexCount, uint32_t vertexStride) const;
bool HasIndexSpace (uint32_t indexCount,  uint32_t indexStride = sizeof(uint32_t)) const;

uint32_t GetVertexBytesUsed() const;  uint32_t GetIndexBytesUsed() const;
uint32_t GetVertexBudget()    const;  uint32_t GetIndexBudget()    const;
```

`TransientAllocation` fields: `IRHIBuffer* buffer`, `void* data` (CPU-writable, valid until
`EndFrame`), `uint32_t offsetBytes`, `uint32_t sizeBytes`, `bool valid`.

### Critical rules

- **Always check `alloc.valid`.** An over-budget request returns a default `TransientAllocation`
  (`valid == false`, all pointers null). It does not grow.
- Allocations are 16-byte aligned (SIMD friendliness); offsets round up to the next 16.
- The `data` pointer is only valid between `BeginFrame` and `EndFrame`. After `EndFrame` it dangles.
- You must `BeginFrame` before allocating — if the buffer is not mapped, `AllocateVertices`/`Indices`
  return `valid == false` (they check `m_vertexMapped`/`m_indexMapped`).
- Bind for drawing at `alloc.buffer` + `alloc.offsetBytes`.

### Typical lifecycle (matches `RHIDeviceBase` + backends)

```cpp
// One instance per RHI device (see RHIDeviceBase.h:45). Backend pumps it:
allocator.Initialize(device);                 // startup
// --- each frame, inside the backend's BeginFrame ---
allocator.BeginFrame(device);
auto v = allocator.AllocateVertices(n, sizeof(Vertex));
if (v.valid) { std::memcpy(v.data, verts, v.sizeBytes); /* draw at v.buffer + v.offsetBytes */ }
allocator.EndFrame(device);                   // inside the backend's EndFrame
```

---

## VersionedHandle + HandleAllocator — use-after-free-safe indirection

**What it is:** a 64-bit handle (`uint32_t index` + `uint32_t generation`) plus an allocator that hands
out slots and bumps the slot's generation every time it is freed. An old handle to a recycled slot has
a stale generation, so `IsValid()` returns false instead of silently aliasing a different object.

**The bug class it prevents:** the classic "recycled-slot use-after-free / ABA". Without generations,
freeing slot 7 and reallocating it hands the same index to a new object; an old reference to slot 7
would now read/write the wrong object with no error. The generation counter makes the stale handle
detectably invalid in O(1). This is distinct from `OpaqueHandle`, which wraps a raw `void*`.

### API (verified against the header)

```cpp
struct VersionedHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
    constexpr bool operator==(const VersionedHandle&) const = default;
    constexpr bool IsNull() const noexcept;                 // index==UINT32_MAX && generation==0
    static constexpr VersionedHandle Null() noexcept;       // {UINT32_MAX, 0}
};

class HandleAllocator {
    explicit HandleAllocator(uint32_t capacity);            // fixed capacity, all slots free
    [[nodiscard]] VersionedHandle Allocate();               // Null() if capacity exhausted (warns)
    void Free(VersionedHandle handle);                      // bumps generation; double-free is a no-op
    [[nodiscard]] bool IsValid(VersionedHandle handle) const; // index in range AND generation matches
    [[nodiscard]] uint32_t GetActiveCount() const;
    [[nodiscard]] uint32_t GetCapacity() const;
};
```

### Critical rules

- **Fixed capacity, no growth.** `Allocate()` returns `VersionedHandle::Null()` and logs
  `SPARK_LOG_WARN` when the free list is empty. Always check `IsValid()` / `IsNull()` before use.
- **Not thread-safe.** The header states this explicitly; serialize externally if shared.
- `Free()` is safe against double-free and stale handles: a generation mismatch makes it a logged
  no-op (`SPARK_LOG_DEBUG`), and an out-of-range index is a logged no-op (`SPARK_LOG_WARN`).
- The allocator only tracks slot validity — it does **not** store your objects. Keep your object array
  parallel to the handle indices yourself.
- Handles are dense from index 0 upward on first fill (the free list is seeded in reverse so index 0
  is handed out first).

### Pattern

```cpp
HandleAllocator handles(1024);
std::array<Entity, 1024> pool;                 // your parallel storage

VersionedHandle h = handles.Allocate();
if (!h.IsNull()) pool[h.index] = makeEntity();

// later, from anywhere holding h:
if (handles.IsValid(h)) use(pool[h.index]);    // false after Free(h) — no UAF

handles.Free(h);                               // slot reusable, generation bumped
```

---

## LockFreeRingAllocator<N> — producer/consumer ring

**What it is:** a fixed-size (compile-time `N`, must be a power of two) ring buffer. `Allocate(size)`
writes a 4-byte size prefix then the payload and advances a write cursor; `Free()` advances a read
cursor past the oldest record (FIFO). Wrap-around at the end of the buffer is handled with a zero-size
"skip" marker.

**Why it exists:** for streaming variable-size records (the header suggests per-frame render command
data) from one thread to another without a mutex and without per-record heap allocation.

### API (verified against the header)

```cpp
template <size_t CapacityBytes = (1 << 20)>    // 1 MB default; must be power of 2 (static_assert)
class LockFreeRingAllocator {
    static constexpr size_t CAPACITY;          // = CapacityBytes
    static constexpr size_t HEADER_SIZE;       // = sizeof(uint32_t)

    void*  Allocate(size_t size);   // nullptr if ring full; payload ptr follows a 4-byte size prefix
    void   Free();                  // advance read cursor past ONE record, in FIFO order
    void   Reset();                 // zero both cursors — NOT thread-safe, call only when idle
    size_t GetUsedBytes() const;    size_t GetFreeBytes() const;    bool IsEmpty() const;

    struct Metrics { uint64_t totalAllocations, totalFrees, totalBytesAllocated; size_t currentUsed; };
    Metrics GetMetrics() const;
};
```

### Thread-safety — read this carefully (do not over-trust "lock-free")

What the code actually guarantees, from reading the atomics:

- It is a **single-producer / single-consumer (SPSC)** structure. Exactly **one** thread may call
  `Allocate()` (the producer/writer) and exactly **one** thread may call `Free()` (the consumer). The
  header's phrase "single-writer/multi-reader" refers to multiple threads **reading the payload bytes**
  of already-published records — it does **not** license concurrent `Allocate` calls or concurrent
  `Free` calls.
- Ordering: `Allocate` publishes the new write cursor with `memory_order_release`; consumers observe it
  with `memory_order_acquire`. That release/acquire pair is what makes written payload bytes visible to
  the reader. The write-side load of its own cursor is `relaxed` (safe: single producer).
- **`Free()` must be called in allocation (FIFO) order**, once per record. It reads the size prefix to
  know how far to advance. Calling it more times than there are records, or out of order, corrupts the
  cursors.
- **`Reset()` is not thread-safe** — call only when no producer/consumer is active.
- **The `Metrics` counters (`m_totalAllocations`, `m_totalFrees`, `m_totalBytesAllocated`) are plain
  non-atomic `uint64_t`.** `Allocate` and `Free` mutate them without synchronization, and `GetMetrics`
  reads them non-atomically. Reading metrics from a third thread concurrently with Allocate/Free is a
  data race — use metrics only for single-threaded diagnostics or when quiesced.
- Cursors are `alignas(64)` to avoid false sharing between producer and consumer cache lines.

> Verification caveat: as of 2026-07-07 there are **no multithreaded tests** for this class
> (`Tests/TestLockFreeRingAllocator*.cpp` are single-threaded) and **no engine call sites**. The SPSC
> guarantees above are read from the source, not proven by a stress test. Treat concurrent use as
> `candidate` and validate under TSan (`build-linux-tsan` CI job) before relying on it.

### Critical rules

- `N` must be a power of two (compile-time `static_assert`).
- `Allocate()` returns `nullptr` when full (including when a wrap-skip would not fit) — null-check.
- The returned pointer is the payload, just past the 4-byte size header; do not read/write the header.
- Do not mix this up with `FrameAllocator`: the ring is FIFO with per-record `Free()`, whereas
  `FrameAllocator` frees everything at once with `Reset()`.

---

## Common mistakes checklist

- [ ] Storing a non-trivially-destructible type in a `FrameAllocator` (destructor never runs).
- [ ] Holding a `FrameAllocator`/`TransientBufferAllocator` `data` pointer across `Reset()`/`EndFrame`.
- [ ] Forgetting to check `alloc.valid` (TransientBuffer), `!= nullptr` (FrameAllocator, ring), or
      `IsValid()`/`IsNull()` (handles) — none of these grow; they fail by returning empty/null.
- [ ] Calling `TransientBufferAllocator::Allocate*` before `BeginFrame` (buffer unmapped -> invalid).
- [ ] Treating `LockFreeRingAllocator` as multi-producer or multi-consumer — it is SPSC only.
- [ ] Reading `LockFreeRingAllocator::GetMetrics()` from another thread during live Allocate/Free.
- [ ] Using a handle's `index` after `Free()` without an `IsValid()` guard.

---

## Provenance and maintenance

Facts verified 2026-07-07 against the repo. Re-verify with:

```bash
# API signatures (the four headers are the ground truth):
sed -n '38,168p'  SparkEngine/Source/Utils/FrameAllocator.h
sed -n '30,155p'  SparkEngine/Source/Utils/LockFreeRingAllocator.h
sed -n '41,157p'  SparkEngine/Source/Utils/VersionedHandle.h
sed -n '60,240p'  SparkEngine/Source/Graphics/RHI/TransientBufferAllocator.h

# Real (non-test) call sites — re-run to catch new adopters:
grep -rn "FrameAllocator"            SparkEngine SparkEditor GameModules --include=*.h --include=*.cpp
grep -rn "TransientBufferAllocator"  SparkEngine --include=*.h --include=*.cpp
grep -rn "LockFreeRingAllocator"     SparkEngine SparkEditor GameModules --include=*.h --include=*.cpp
grep -rn "HandleAllocator\|VersionedHandle" SparkEngine SparkEditor GameModules --include=*.h --include=*.cpp

# Confirm the ring still has no multithreaded coverage (expect no matches):
grep -rn "std::thread" Tests/TestLockFreeRingAllocator*.cpp
```

If a new engine consumer of `LockFreeRingAllocator` or `HandleAllocator` appears, promote it from
`candidate` to a documented real usage in the decision guide above. Related: the global **memory-hunt**
skill covers leak/UAF investigation methodology (not duplicated here).
