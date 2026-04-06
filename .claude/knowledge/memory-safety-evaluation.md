# Memory Safety Evaluation & C++26 Forward-Compatibility

**Type:** Decision  
**Status:** Active  
**Created:** 2026-04-06  

## Context

Evaluated whether SparkEngine should wait for C++26 memory safety features or build custom safety utilities now. Decision: build now, design for C++26 forward-compatibility.

## What Already Existed

- Smart pointers ubiquitous (173+ files, unique_ptr/shared_ptr/ComPtr)
- MemoryDebugger (Utils/MemoryDebugger.h) — leak detection, allocation tracking
- MemoryMonitor (Utils/MemoryMonitor.h) — anomaly detection, budget enforcement
- ScopeGuard (Utils/ScopeGuard.h) — ScopeExit/Success/Fail RAII guards
- Assert.h — 13 assertion macros (ASSERT_BOUNDS, ASSERT_NOT_NULL, VERIFY, etc.)
- MemoryIntegrity (Engine/Security/MemoryIntegrity.h) — anti-tamper, branch guards
- Platform.h — C++26 feature detection (SPARK_HAS_CONTRACTS, SPARK_HAS_REFLECTION)
- CI sanitizers: ASan+UBSan+LSan, TSan, MSan (all in build.yml)
- clang-tidy with bugprone/modernize/performance checks
- FrameAllocator, LockFreeRingAllocator, ObjectPool custom allocators

## What Was Added

### New Headers
- **Core/Contracts.h** — SPARK_EXPECTS/SPARK_ENSURES/SPARK_INVARIANT macros that bridge to C++26 `[[pre/post/assert]]` attributes when SPARK_HAS_CONTRACTS is defined, fall back to ASSERT_MSG in debug
- **Core/NonNull.h** — Spark::NonNull<T*> wrapper: compile-time nullptr_t rejection, debug-mode null assertion, zero overhead in Release
- **Core/SafeCast.h** — Spark::NarrowCast<To>(from) for checked narrowing, Spark::CheckedCast<Derived*>(base) for validated downcasts

### Compiler Hardening (CMakeLists.txt)
- `-fstack-protector-strong` on GCC/Clang (all configs)
- `-D_FORTIFY_SOURCE=2` on GCC/Clang (optimized builds only)
- `-Wl,-z,relro,-z,now` on Linux (full RELRO)
- Documented MSVC `/GS` is enabled by default

### Wired In
- SPARK_EXPECTS on EngineContext::RegisterSystem() (null check)
- SPARK_EXPECTS on FrameAllocator::AllocRaw() (alignment power-of-2 check)

## Codebase Integration (Phase 2)

Contracts, NonNull, and SafeCast are wired in across 15+ files:

**Contracts (SPARK_EXPECTS):**
- FixedTimestepAccumulator (Initialize, SetFixedTimestep, Advance)
- ModuleManager (LoadModule, InitializeAll, ResizeAll)
- EngineContext (RegisterSystem), FrameAllocator (AllocRaw)
- ObjectPool (constructor), RingBuffer (operator[])
- ConstantBufferRing (Initialize), RHIAdapter (Initialize)
- MaterialSystem (Initialize)

**NonNull<T*>:**
- ECSystems.h: RenderSystem, PhysicsUpdateSystem, AudioUpdateSystem constructors

**CheckedCast:**
- MovementSystem.cpp: 10 generator downcasts
- Sequencer.cpp: 4 track downcasts

**NarrowCast:**
- SplinePath.cpp: 9 size_t→int conversions

**Wiki:** wiki/Memory-Safety.md (comprehensive, linked from sidebar and Memory-Management-Patterns.md)

## Known Gaps (Not Addressed — By Design)

- **Borrow checker/lifetime tracker** — poor C++ fit, wait for C++26 Profiles
- **EngineContext void* rewrite** — API is already type-safe via GetTypeId<T>()
- **reinterpret_cast in socket/GPU/serialization** — necessary for those domains
- **Thread safety annotations** — better done via Clang's `__attribute__((guarded_by))`
- **53+ files with reinterpret_cast, 119+ files with void*** — inherent to C++ interop

## Key Observation

C++26 contracts won't ship in compilers until ~2027-2028. Profiles (safe subset) may not even make C++26. The bridge macros ensure smooth adoption when they arrive.
