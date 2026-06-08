# Memory Safety Evaluation & C++26 Forward-Compatibility

> **Audience:** Programmers
>
> **Thread Context:** Contracts/NonNull/SafeCast are zero-overhead in Release and operate wherever the wrapped code runs; no dedicated thread. Thread-safety annotations are explicitly deferred (see Known Gaps).
>
> **Platform/Backend Scope:** Compiler hardening differs per toolchain — `-fstack-protector-strong`/`_FORTIFY_SOURCE=2`/full RELRO on GCC/Clang; MSVC `/GS` is on by default. CI sanitizers (ASan+UBSan+LSan, TSan, MSan) run on Linux.

## Overview

This page records the decision (made 2026-04-06) on whether SparkEngine should wait for C++26 memory-safety features or build custom safety utilities now. **Decision: build now, design for C++26 forward-compatibility** via bridge macros that adopt the standard attributes automatically once compilers ship them.

The comprehensive how-to guide for using these utilities lives in [Memory Safety](Memory-Safety.md); this page is the rationale and the integration record.

## What already existed

- Smart pointers ubiquitous across the codebase (`unique_ptr`/`shared_ptr`/`ComPtr`).
- `MemoryDebugger` (`Utils/MemoryDebugger.h`) — leak detection, allocation tracking.
- `MemoryMonitor` (`Utils/MemoryMonitor.h`) — anomaly detection, budget enforcement.
- `ScopeGuard` (`Utils/ScopeGuard.h`) — ScopeExit/Success/Fail RAII guards.
- `Assert.h` — assertion macros (`ASSERT_BOUNDS`, `ASSERT_NOT_NULL`, `VERIFY`, etc.).
- `MemoryIntegrity` (`Engine/Security/MemoryIntegrity.h`) — anti-tamper, branch guards (see [Memory Integrity System](Memory-Integrity-System.md)).
- `Platform.h` — C++26 feature detection (`SPARK_HAS_CONTRACTS`, `SPARK_HAS_REFLECTION`).
- CI sanitizers: ASan+UBSan+LSan, TSan, MSan.
- clang-tidy with bugprone/modernize/performance checks.
- Custom allocators: `FrameAllocator`, `LockFreeRingAllocator`, `ObjectPool`.

## What was added

### New headers (all confirmed present 2026-06-08)

- **`Core/Contracts.h`** — `SPARK_EXPECTS`/`SPARK_ENSURES`/`SPARK_INVARIANT` macros that bridge to C++26 `[[pre]]`/`[[post]]`/`[[assert]]` attributes when `SPARK_HAS_CONTRACTS` is defined, falling back to `ASSERT_MSG` in debug.
- **`Core/NonNull.h`** — `Spark::NonNull<T*>`: compile-time `nullptr_t` rejection, debug-mode null assertion, zero overhead in Release.
- **`Core/SafeCast.h`** — `Spark::NarrowCast<To>(from)` for checked narrowing, `Spark::CheckedCast<Derived*>(base)` for validated downcasts.

### Compiler hardening (CMakeLists.txt, confirmed present)

- `-fstack-protector-strong` on GCC/Clang (all configs).
- `_FORTIFY_SOURCE=2` on GCC/Clang (optimized builds only).
- `-Wl,-z,relro,-z,now` on Linux (full RELRO).
- MSVC `/GS` documented as enabled by default.

## Codebase integration

Contracts, NonNull, and SafeCast are wired across many files.

**Contracts (`SPARK_EXPECTS`/`SPARK_ENSURES`):** FixedTimestepAccumulator, ModuleManager, EngineContext (`RegisterSystem`), FrameAllocator (`AllocRaw`), ObjectPool (ctor), RingBuffer (`operator[]`), ConstantBufferRing (`Initialize`), RHIAdapter (`Initialize`), MaterialSystem (`Initialize`).

**`NonNull<T*>`:** `ECSystems.h` — RenderSystem, PhysicsUpdateSystem, AudioUpdateSystem constructors.

**`CheckedCast`:** MovementSystem.cpp (generator downcasts), Sequencer.cpp (track downcasts).

**`NarrowCast`:** SplinePath.cpp (size_t→int conversions).

> A repo-wide grep on 2026-06-08 confirms these utilities are actively referenced across engine source (`SPARK_EXPECTS`/`SPARK_ENSURES`, `NonNull<`, `CheckedCast`/`NarrowCast` all return live call sites). Exact counts will drift as integration expands — treat the file list above as representative, not exhaustive.

## Known gaps (not addressed — by design)

| Gap | Rationale |
|---|---|
| Borrow checker / lifetime tracker | Poor C++ fit; wait for C++26 Profiles |
| `EngineContext` `void*` rewrite | API already type-safe via `GetTypeId<T>()` |
| `reinterpret_cast` in socket/GPU/serialization | Necessary for those domains |
| Thread-safety annotations | Better via Clang `__attribute__((guarded_by))` |
| Many files with `reinterpret_cast` / `void*` | Inherent to C++ interop |

## Key observation

C++26 contracts are not expected in compilers until ~2027–2028, and Profiles (the safe subset) may not even land in C++26. The bridge macros in `Contracts.h` ensure smooth, edit-free adoption when the attributes arrive.

## Source & Freshness

- Original entry: `.claude/knowledge/memory-safety-evaluation.md` (created 2026-04-06).
- Verified against codebase 2026-06-08.

Status changes / verifications found during freshening:

- `Core/Contracts.h`, `Core/NonNull.h`, `Core/SafeCast.h` — all confirmed present.
- Compiler hardening confirmed in `CMakeLists.txt` (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, `-Wl,-z,relro,-z,now`).
- `SPARK_EXPECTS`/`SPARK_ENSURES`, `NonNull<`, and `CheckedCast`/`NarrowCast` all return live references across engine source.
- The companion how-to page `wiki/advanced/Memory-Safety.md` exists and is linked from the sidebar.
- No status changes; entry is current.

## Related Pages

- [Memory Safety](Memory-Safety.md)
- [Memory Integrity System](Memory-Integrity-System.md)
- [Memory Management Patterns](Memory-Management-Patterns.md)
- [Error Handling Patterns](Error-Handling-Patterns.md)
