# Memory Safety

SparkEngine implements a multi-layered memory safety strategy using C++23 features with forward-compatible bridges to C++26. This page covers the type-safe utilities, contract annotations, safe cast wrappers, and compiler hardening that protect against common memory errors.

**Source:** `SparkEngine/Source/Core/Contracts.h`, `NonNull.h`, `SafeCast.h`

---

## Architecture Overview

```
                         Compile-Time Safety
                    ┌─────────────────────────┐
                    │  NonNull<T*>             │  Type-level null rejection
                    │  NarrowCast<To>          │  Checked numeric narrowing
                    │  CheckedCast<Derived*>   │  Validated pointer downcast
                    └───────────┬─────────────┘
                                │
                     Debug-Time Safety
                    ┌───────────┴─────────────┐
                    │  SPARK_EXPECTS(expr)     │  Precondition assertions
                    │  SPARK_ENSURES(expr)     │  Postcondition assertions
                    │  SPARK_INVARIANT(expr)   │  Class invariant checks
                    │  ASSERT_BOUNDS / NOT_NULL│  Existing assert macros
                    └───────────┬─────────────┘
                                │
                    Runtime / CI Safety
                    ┌───────────┴─────────────┐
                    │  ASan + UBSan + LSan     │  Address/undefined/leak
                    │  TSan                    │  Thread race detection
                    │  MSan                    │  Uninitialized memory
                    │  MemoryDebugger          │  Allocation tracking
                    │  MemoryMonitor           │  Anomaly detection
                    │  MemoryIntegrity         │  Anti-tamper scanning
                    └───────────┬─────────────┘
                                │
                      Compiler Hardening
                    ┌───────────┴─────────────┐
                    │  -fstack-protector-strong│  Stack overflow detection
                    │  _FORTIFY_SOURCE=2       │  libc buffer checks
                    │  -Wl,-z,relro,-z,now     │  Full RELRO (GOT protection)
                    │  /GS (MSVC default)      │  Buffer security check
                    └─────────────────────────┘
```

## Source Files

| File | Responsibility |
|------|---------------|
| `Core/Contracts.h` | C++26 contract bridge macros (SPARK_EXPECTS/ENSURES/INVARIANT) |
| `Core/NonNull.h` | Non-null pointer wrapper with debug enforcement |
| `Core/SafeCast.h` | NarrowCast and CheckedCast type-safe utilities |
| `Core/Platform.h` | SPARK_HAS_CONTRACTS feature detection macro |
| `Utils/Assert.h` | 13 assertion macros (fallback for contracts) |
| `Utils/MemoryDebugger.h` | Allocation tracking, leak detection, double-free detection |
| `Utils/MemoryMonitor.h` | Budget enforcement, growth tracking, anomaly detection |
| `Engine/Security/MemoryIntegrity.h` | Runtime code scanning and branch guards |
| `CMakeLists.txt` | Compiler hardening flags |

---

## Contract Macros

**Source:** `Core/Contracts.h`

Contract macros annotate function preconditions, postconditions, and class invariants. When C++26 compiler support arrives (`SPARK_HAS_CONTRACTS`), they automatically switch to standard `[[pre:]]`/`[[post:]]`/`[[assert:]]` attributes. Until then, they fall back to debug-mode `ASSERT_MSG` macros.

### Macro Reference

| Macro | Purpose | C++26 Equivalent |
|-------|---------|-----------------|
| `SPARK_EXPECTS(expr)` | Precondition — checked at function entry | `[[pre: expr]]` |
| `SPARK_ENSURES(expr)` | Postcondition — checked before return | `[[post: expr]]` |
| `SPARK_INVARIANT(expr)` | Class invariant — checked at key points | `[[assert: expr]]` |

### Usage

```cpp
#include "Core/Contracts.h"

void FixedTimestepAccumulator::Initialize(float fixedDt)
{
    SPARK_EXPECTS(fixedDt > 0.0f);       // precondition: valid timestep
    // ... implementation ...
    SPARK_ENSURES(m_initialized);         // postcondition: state is valid
}

void FrameAllocator::AllocRaw(size_t bytes, size_t alignment)
{
    SPARK_EXPECTS(alignment > 0 && (alignment & (alignment - 1)) == 0);  // power of 2
    // ... implementation ...
}

void ModuleManager::ResizeAll(int width, int height)
{
    SPARK_EXPECTS(width > 0 && height > 0);
    // ... implementation ...
}
```

### Where Contracts Are Wired In

| System | File | Contract |
|--------|------|----------|
| EngineContext | `Core/EngineContext.h` | `RegisterSystem()` — system ptr not null |
| FrameAllocator | `Utils/FrameAllocator.h` | `AllocRaw()` — alignment is power of 2 |
| FixedTimestep | `Core/FixedTimestepAccumulator.cpp` | `Initialize/SetFixedTimestep/Advance` — dt > 0, frameDt >= 0 |
| ModuleManager | `Core/ModuleManager.cpp` | `LoadModule` — path not empty; `InitializeAll` — context not null; `ResizeAll` — dimensions > 0 |
| ObjectPool | `Utils/ObjectPool.h` | Constructor — capacity > 0 |
| RingBuffer | `Utils/RingBuffer.h` | `operator[]` — index < count |
| ConstantBufferRing | `Graphics/ConstantBufferRing.h` | `Initialize` — device not null, capacity > 0 |
| RHIAdapter | `Graphics/RHI/RHIAdapter.cpp` | `Initialize` — device not null |
| MaterialSystem | `Graphics/MaterialSystem.cpp` | `Initialize` — device and context not null |

---

## NonNull Pointer Wrapper

**Source:** `Core/NonNull.h`

`Spark::NonNull<T*>` documents non-null intent at the type level and enforces it with a debug-mode assertion. In release builds, it compiles to a raw pointer with zero overhead.

### Interface

```cpp
namespace Spark
{
    template <typename T> class NonNull
    {
    public:
        constexpr NonNull(T ptr);            // debug-asserts non-null
        NonNull(std::nullptr_t) = delete;    // compile error on nullptr

        constexpr operator T() const;        // implicit conversion to raw ptr
        constexpr element_type& operator*() const;
        constexpr T operator->() const;
        constexpr T Get() const;
    };

    template <typename T> NonNull(T) -> NonNull<T>;  // CTAD
}
```

### Usage

```cpp
#include "Core/NonNull.h"

// Function signature documents that graphics must not be null
RenderSystem(Spark::NonNull<GraphicsEngine*> graphics)
    : m_graphics(graphics) {}

// Callers pass raw pointers — NonNull checks in debug
auto* gfx = context->GetGraphics();
auto renderSys = RenderSystem(gfx);   // asserts non-null in debug
```

### Where NonNull Is Wired In

| System | File | Parameter |
|--------|------|-----------|
| RenderSystem | `ECS/Systems/ECSystems.h` | Constructor `graphics` parameter |
| PhysicsUpdateSystem | `ECS/Systems/ECSystems.h` | Constructor `physics` parameter |
| AudioUpdateSystem | `ECS/Systems/ECSystems.h` | Constructor `audio` parameter |

### When to Use NonNull

- Constructor parameters stored as non-owning member pointers
- Functions that immediately dereference their pointer parameter
- Functions documented as requiring non-null ("must not be null")
- `SetXxx(T* ptr)` methods where null is invalid

### When NOT to Use NonNull

- Return types (keep as raw `T*` with nullable semantics)
- Optional parameters that accept nullptr
- Internal/private methods where null is already impossible

---

## Safe Cast Utilities

**Source:** `Core/SafeCast.h`

### NarrowCast

Checked narrowing numeric conversion. In debug builds, asserts that the round-trip cast preserves the value (catches overflow, truncation, and sign changes). In release builds, compiles to `static_cast`.

```cpp
#include "Core/SafeCast.h"

// Safe narrowing — asserts in debug if value is lost
int count = Spark::NarrowCast<int>(m_points.size());

// Signed/unsigned mismatch — also checks sign preservation
auto idx = Spark::NarrowCast<int>(unsignedValue);
```

### CheckedCast

Validated pointer downcast. In debug builds, verifies the cast via `dynamic_cast` and asserts on mismatch. In release builds, compiles to `static_cast`.

```cpp
#include "Core/SafeCast.h"

// Safe downcast — verifies type in debug
auto* wander = Spark::CheckedCast<RandomWanderGenerator*>(gen);
auto* eventTrack = Spark::CheckedCast<EventTrack*>(track.get());
```

### Where Safe Casts Are Wired In

| System | File | Replacement |
|--------|------|-------------|
| SplinePath | `Utils/SplinePath.cpp` | 9x `NarrowCast<int>(m_points.size())` |
| MovementSystem | `Engine/AI/MovementSystem.cpp` | 10x `CheckedCast<XxxGenerator*>(gen)` |
| Sequencer | `Engine/Cinematic/Sequencer.cpp` | 4x `CheckedCast<XxxTrack*>(track.get())` |

### When to Use Safe Casts

**NarrowCast:** Any `static_cast` between numeric types where the destination is narrower than the source (`size_t` to `int`, `double` to `float`, `int` to `uint8_t`). Avoid in hot loops.

**CheckedCast:** Any `static_cast` downcast in a class hierarchy where you "know" the type but want debug verification. Not for `reinterpret_cast` (socket/GPU/serialization code).

---

## Compiler Hardening

The build system enables multiple layers of compiler-level protection:

### GCC / Clang

| Flag | Purpose |
|------|---------|
| `-fstack-protector-strong` | Detects stack buffer overflows via canary values |
| `-D_FORTIFY_SOURCE=2` | Compile-time + runtime buffer overflow checks for libc functions (Release only) |
| `-Wl,-z,relro,-z,now` | Full RELRO — GOT becomes read-only after relocation (Linux) |
| `-fno-omit-frame-pointer` | Preserves frame pointers for backtrace resolution |
| `_GLIBCXX_ASSERTIONS` | STL container bounds checking (Debug only) |

### MSVC

| Flag | Purpose |
|------|---------|
| `/GS` | Buffer security check (enabled by default) |
| `/RTC1` | Stack frame validation + uninitialized variable detection (Debug) |
| `_ITERATOR_DEBUG_LEVEL=1` | Iterator bounds checking without extreme slowdown |

### CI Sanitizers

| Sanitizer | What It Detects | CI Job |
|-----------|----------------|--------|
| ASan + UBSan + LSan | Buffer overflows, undefined behavior, memory leaks | `build-linux-asan` |
| TSan | Data races, deadlocks | `build-linux-tsan` |
| MSan | Uninitialized memory reads | `build-linux-msan` |

---

## C++26 Forward-Compatibility

SparkEngine detects C++26 features via macros in `Core/Platform.h`:

| Macro | C++26 Feature | Current Status |
|-------|--------------|----------------|
| `SPARK_HAS_CONTRACTS` | `[[pre:]]` / `[[post:]]` / `[[assert:]]` | Detected, not yet available |
| `SPARK_HAS_REFLECTION` | `std::meta::reflect` | Detected, not yet available |
| `SPARK_HAS_PACK_INDEXING` | Pack indexing expressions | Detected, not yet available |

When compilers ship C++26 contract support, the `SPARK_EXPECTS`/`SPARK_ENSURES`/`SPARK_INVARIANT` macros will automatically switch to standard attributes with no code changes required.

**Profiles (safe subset):** The C++26 Profiles proposal aims to add opt-in safety modes (bounds checking, null checking, lifetime analysis). If accepted, SparkEngine's existing safety utilities will complement — not conflict with — profiles.

---

## Best Practices

1. **Use SPARK_EXPECTS for public API boundaries.** Document what callers must guarantee.
2. **Use NonNull for constructor parameters.** If a system requires a pointer at construction, make it `NonNull`.
3. **Use NarrowCast for size_t-to-int conversions.** These are the most common source of silent truncation.
4. **Use CheckedCast for type-switch downcasts.** Anywhere you switch on a type enum then downcast.
5. **Don't replace reinterpret_cast.** Socket, GPU, and serialization casts are inherently low-level.
6. **Don't use NonNull for return types.** Return raw `T*` — callers check for null.
7. **Don't add contracts to hot loops.** They are for API boundaries, not per-element checks.

---

## Testing

The safety utilities have dedicated tests:

| Test File | Tests |
|-----------|-------|
| `Tests/TestContracts.cpp` | Contract macros pass on valid conditions, complex expressions |
| `Tests/TestSafetyCoreUtils.cpp` | NonNull construction, conversion, CTAD; NarrowCast lossless conversions; CheckedCast valid/null/const |

Run with:
```bash
cd build && ctest --output-on-failure --no-tests=error
```

---

## See Also

- [Memory Management Patterns](Memory-Management-Patterns.md) — Ownership rules, allocators, debugging tools
- [Memory Integrity](../subsystems/Memory-Integrity.md) — Anti-tamper system, branch guards
- [Error Handling Patterns](Error-Handling-Patterns.md) — SPARK_VALIDATE, SPARK_CHECK patterns
- [Threading Model](Threading-Model.md) — Thread safety rules and mutex conventions
- [Testing](Testing.md) — CI sanitizer configuration and test infrastructure
