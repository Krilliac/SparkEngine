# SparkEngine Stability — Gap Analysis

> **Scope**: All source files under `SparkEngine/Source/`, `SparkEditor/Source/`, and `Tests/`
> **Date**: 2026-03-09
> **Methodology**: Static analysis patterns across the entire codebase — raw pointer usage, error handling, thread safety, resource management, crash handling, and defensive programming.
> Each gap is assigned a severity: **Critical** (can cause crash), **Major** (can cause incorrect behavior), **Moderate** (defensive improvement), **Minor** (code quality).

---

## Critical Gaps

### GAP-ST01 — 141 Raw `new` Allocations Across 54 Files

**Impact**: The project's coding standard mandates `std::unique_ptr` for ownership and no naked `new`/`delete`, but 141 raw `new` allocations exist across 54 source files, with 32 corresponding `delete` calls across 7 files. This mismatch (141 `new` vs. 32 `delete`) indicates potential memory leaks.

**Key offenders**:
- `Physics/PhysicsSystem.cpp`: 19 `new` calls (Bullet Physics objects — `btBoxShape`, `btRigidBody`, `btHingeConstraint`, etc.) with 10 `delete` calls
- `Physics/PhysicsSystem.h`: 10 `new` calls in inline methods
- `SceneManager/SceneManager.h`: 6 `new` calls
- `Graphics/RHI/D3D11/D3D11Device.cpp`: 6 `new` / 6 `delete`
- `Graphics/RHI/Vulkan/VulkanDevice.cpp`: 6 `new` / 6 `delete`
- `Graphics/RHI/OpenGL/OpenGLDevice.cpp`: 6 `new` / 6 `delete`
- `Utils/ObjectPool.h`: 6 `new` / 2 `delete` (pool allocation, potential leak on pool destruction)

**What is needed**:
- Wrap Bullet Physics allocations in RAII wrappers or custom deleters with `unique_ptr`
- Audit `ObjectPool.h` for proper cleanup on destruction
- Convert `SceneManager` allocations to smart pointers
- Enforce the coding standard via clang-tidy `cppcoreguidelines-owning-memory` check

---

### GAP-ST02 — Global Pointers Create Initialization/Shutdown Order Hazards

**Files**: 16 files contain `g_*` global variables

**Impact**: Multiple subsystems use raw global pointers (`g_physicsSystem`, `g_graphics`, `g_input`, etc.) that create fragile initialization and shutdown ordering. If subsystem A's destructor uses global B after B has been destroyed, undefined behavior occurs.

**Key globals found**:
- `Physics/PhysicsSystem.cpp`: `PhysicsSystem* g_physicsSystem = nullptr`
- `Core/SparkEngine.cpp` / `.h`: Multiple `g_*` pointers to engine subsystems
- `Utils/CrashHandler.cpp`: `static CrashConfig g_cfg`, `static bool g_triggerCrashOnAssert`
- `Utils/SparkConsole.cpp`: Global console state

**What is needed**: Migrate all globals to `EngineContext` service locator. Define explicit initialization and shutdown order. Use `EngineContext::Shutdown()` to tear down in reverse initialization order.

---

### GAP-ST03 — No ECS Iterator Safety During Entity Destruction

**Files**: `Engine/ECS/Components.h` (World class, line 83)

**Impact**: `World::DestroyEntity()` calls `m_registry.destroy()` immediately. If called during a system's entity iteration loop, EnTT invalidates the iterator, causing undefined behavior (crash or skipped entities).

**Evidence**:
```cpp
void DestroyEntity(EntityID entity) { m_registry.destroy(entity); }
```

No deferred destruction queue exists. The `LifecycleSystem` death callback may destroy entities during iteration.

**What is needed**: Implement `World::QueueDestroyEntity()` that adds to a pending list. Call `World::FlushDestroyQueue()` between system updates in `SystemManager::UpdateAll()`.

---

### GAP-ST04 — Fixed-Size Buffers in Logging and Error Handling

**Files**:
- `Utils/SparkError.h` (line 74: `char userMsg[1024]`, line 94: `char fullMsg[2048]`)
- `Utils/FileLogger.h` (line 306: `char buf[2048]`)

**Impact**: Log messages are silently truncated at ~1-2KB via `snprintf`. While `snprintf` prevents buffer overflow, the truncation can lose critical diagnostic information during crashes. Stack traces, long file paths, and serialization dumps regularly exceed 2KB.

**What is needed**: Use `std::string` with `std::format` or `vsnprintf` with dynamic allocation for messages that exceed the buffer.

---

### GAP-ST05 — 130 `reinterpret_cast` Usages

**Impact**: 130 `reinterpret_cast` occurrences across 21 files indicate extensive type punning. While some are necessary (COM interfaces, serialization), excessive use increases the risk of undefined behavior.

**Key concerns**:
- `Engine/SaveSystem/SaveSystem.cpp`: 18 casts (binary serialization)
- `Engine/AI/NavMesh.cpp`: 15 casts (binary navmesh I/O)
- `Graphics/AssetPipeline.cpp`: 12 casts (asset loading)
- `Graphics/Shader.cpp`: 11 casts (shader bytecode)

**What is needed**: Audit each `reinterpret_cast` for strict aliasing violations. Replace with `std::memcpy` for type punning (which is well-defined behavior) or `std::bit_cast` (C++20).

---

### GAP-ST06 — Crash Handler Does Not Flush Logs Before Minidump

**Files**:
- `Utils/CrashHandler.cpp`
- `Utils/CrashHandlerHelpers.cpp`

**Impact**: The crash handler generates minidumps on Windows and captures signals on Linux, but does not call `Logger::FlushAll()` (or any log flush) before writing the dump. The last log messages before the crash — often the most diagnostic — are lost.

**Evidence**: CrashHandler includes error reporting via `SparkError` and crash report file generation, but the file logger (if it were active) is not flushed. On crash, buffered log output is lost.

**What is needed**: Add `Logger::FlushAll()` call as the first action in the crash handler, before minidump generation. Ensure the flush is async-signal-safe on Linux (pre-allocate the file buffer).

---

## Major Gaps

### GAP-ST07 — Exception Handling is Inconsistent (112 `catch` Blocks, Concentrated in Few Files)

**Impact**: 112 `catch` blocks exist but are heavily concentrated:
- `Utils/SparkConsole.cpp`: 47 catches (console command parsing)
- `Graphics/GraphicsEngine.cpp`: 23 catches (D3D11 operations)
- `Utils/CrashHandler.cpp`: 7 catches
- `Graphics/MaterialSystem.cpp`: 6 catches
- `Engine/SaveSystem/SaveSystem.cpp`: 5 catches

Most subsystems (Physics, AI, Animation, Networking, Audio, ECS) have ZERO exception handling. If a standard library function throws (e.g., `std::bad_alloc`, `std::out_of_range`), these subsystems will crash with an unhandled exception.

**What is needed**: Add `try`/`catch` at subsystem boundaries (initialization, per-frame update entry points). Catch `std::exception&` and log the error rather than crashing. Do NOT add catch-all blocks deep in the code — only at boundaries.

---

### GAP-ST08 — Physics System Is Main-Thread Only But Not Enforced

**Files**: `Physics/PhysicsSystem.h`, `Physics/PhysicsSystem.cpp`

**Impact**: The CLAUDE.md documents "PhysicsSystem — main thread only" but there is no runtime assertion or guard. If any system calls physics methods from a background thread, Bullet Physics will exhibit data races and crash.

**What is needed**: Add `SPARK_ASSERT(IsMainThread())` in `PhysicsSystem::Update()`, `CreateBody()`, `DestroyBody()`, `Raycast()`, and all other public methods.

---

### GAP-ST09 — Thread Safety of `SimpleConsole` Is Incomplete

**Files**: `Utils/SparkConsole.h`, `Utils/SparkConsole.cpp`

**Impact**: CLAUDE.md states `SimpleConsole` is thread-safe (mutex), but the mutex only protects `m_logHistory`. The `Print()`/`PrintLine()` methods call Win32 console APIs (`SetConsoleTextAttribute`, `WriteConsole`) without synchronization. Concurrent calls from multiple threads can interleave output and corrupt color state.

**What is needed**: Extend the mutex to cover all console output operations, or use a lock-free output queue.

---

### GAP-ST10 — NetworkManager Queue Mutex May Deadlock

**Files**: `Engine/Networking/NetworkManager.h`

**Impact**: `m_queueMutex` is used for both `m_outgoingQueue` and `m_incomingQueue`. If `ProcessIncoming()` calls a message handler that calls `SendMessage()` (which also locks `m_queueMutex`), a deadlock occurs if the mutex is not recursive.

**Evidence**: `m_queueMutex` is declared as `std::mutex` (non-recursive). The handler pattern `RegisterHandler(type, handler)` means user code runs under the lock.

**What is needed**: Either:
- Use `std::recursive_mutex` (simple but masks design issues)
- Or copy incoming messages to a local vector under the lock, release the lock, then dispatch handlers (better design)

---

### GAP-ST11 — GraphicsEngine Uses `std::atomic` for Frame State But No Memory Order Specified

**Files**: `Graphics/GraphicsEngine.h`

**Impact**: CLAUDE.md notes "GraphicsEngine — main thread render, `std::atomic` frame state." If atomic operations use the default `memory_order_seq_cst`, this is safe but potentially slower than necessary. However, if any relaxed orderings are used without careful analysis, data races on non-atomic data visible through the atomic flag can occur.

**What is needed**: Audit all atomic operations for correct memory ordering. Document the threading model and which data is protected by each atomic.

---

## Moderate Gaps

### GAP-ST12 — No Graceful Recovery from GPU Device Loss

**Files**: `Graphics/GraphicsEngine.cpp`

**Impact**: D3D11 device loss (`DXGI_ERROR_DEVICE_REMOVED`, `DXGI_ERROR_DEVICE_RESET`) can occur due to driver updates, GPU timeouts, or hardware issues. The engine has 23 `catch` blocks in the graphics engine but no explicit device loss recovery path.

**What is needed**: Implement `CheckDeviceState()` per frame. On device loss:
1. Release all D3D11 resources
2. Recreate device and swap chain
3. Reload shaders and textures
4. Restore render state

---

### GAP-ST13 — `ObjectPool` May Leak on Destruction

**File**: `Utils/ObjectPool.h`

**Impact**: 6 `new` calls with only 2 `delete` calls. If the pool's destructor does not clean up all allocated-but-returned objects, memory leaks on pool destruction.

**What is needed**: Ensure `~ObjectPool()` calls `delete` on all allocated objects (both in-use and available).

---

### GAP-ST14 — Module Loading Has Minimal Error Handling

**Files**:
- `Core/ModuleManager.cpp` (4 `new`, 1 `delete`, 8 `reinterpret_cast`)
- `Core/GameModuleLoader.h` (2 `new`, 4 `reinterpret_cast` in `.cpp`)

**Impact**: DLL loading (`LoadLibrary`, `GetProcAddress`) and function pointer casting via `reinterpret_cast` have minimal validation. A corrupt or incompatible game module DLL could crash the engine.

**What is needed**:
- Verify module version/ABI compatibility before calling functions
- Wrap module function calls in SEH (Windows) or signal handlers
- Validate function pointer signatures where possible

---

### GAP-ST15 — SaveSystem Binary Serialization Has 18 `reinterpret_cast` Calls

**File**: `Engine/SaveSystem/SaveSystem.cpp`

**Impact**: Binary serialization uses `reinterpret_cast` for reading/writing structs directly to/from files. This is fragile: struct layout changes, padding differences across compilers, or endianness changes will silently corrupt save files.

**What is needed**: Use field-by-field serialization with explicit byte ordering, or use a serialization library (cereal, flatbuffers, protobuf). At minimum, add a version header to save files and validate it on load.

---

### GAP-ST16 — No Memory Budget or Allocation Tracking

**Files**: `Utils/MemoryDebugger.h`

**Impact**: `MemoryDebugger.h` exists with 17 `printf` calls (per the Logging Gap Analysis) but it's unclear if it tracks actual allocations. There is no global memory budget, no per-subsystem allocation tracking, and no early warning for memory exhaustion.

**What is needed**: Implement allocation tracking with per-subsystem budgets. Log warnings when a subsystem exceeds 80% of its budget. On `std::bad_alloc`, attempt graceful degradation (free caches) before crashing.

---

## Minor Gaps

### GAP-ST17 — No Runtime Validation of Enum Values at API Boundaries

**Impact**: `ASSERT_ENUM_RANGE` exists in `Assert.h` but is debug-only. In release builds, passing an out-of-range enum to a `switch` statement falls through to unexpected behavior.

**What is needed**: Add `default: SPARK_ASSERT_ALWAYS(false)` to all enum `switch` statements that should be exhaustive.

---

### GAP-ST18 — `static` Local Variables in Editor Panels (Data Corruption)

**Files**: `SparkEditor/Source/Panels/InspectorPanel.cpp` (lines 236-238)

**Impact**: As noted in the Editor Gap Analysis (GAP-E09), `static float position[3]` is shared across all entity selections. This is a stability issue: editing entity A's position modifies the same memory that entity B reads, causing data corruption.

---

### GAP-ST19 — Linux Signal Handler Uses Non-Async-Signal-Safe Functions

**File**: `Utils/CrashHandler.cpp`

**Impact**: The Linux signal handler likely calls `fprintf`, `malloc` (via `std::string`), and other functions that are not async-signal-safe. Calling these in a signal handler is undefined behavior per POSIX and can cause deadlocks or secondary crashes.

**What is needed**: In the signal handler, only use async-signal-safe functions (`write`, `_exit`). Defer complex operations (stack trace formatting, file I/O) to a separate process or pre-allocated buffers.

---

### GAP-ST20 — No Watchdog Timer for Frame Hangs

**Impact**: If the main loop hangs (infinite loop in physics, shader compilation stall, deadlock), the application freezes with no diagnostic output. There is no watchdog thread that detects hangs and triggers a crash report.

**What is needed**: Spawn a watchdog thread that monitors the main loop timestamp. If the main loop hasn't advanced in N seconds (e.g., 30s), generate a crash report with the main thread's stack trace.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-ST01 | Critical | 141 raw `new` (32 `delete`) | Memory leaks |
| GAP-ST02 | Critical | Global pointer init order | Shutdown crashes |
| GAP-ST03 | Critical | No deferred entity destruction | Iterator invalidation |
| GAP-ST04 | Critical | Fixed-size log buffers | Truncated diagnostics |
| GAP-ST05 | Critical | 130 `reinterpret_cast` | Undefined behavior risk |
| GAP-ST06 | Critical | Crash handler doesn't flush logs | Lost diagnostics |
| GAP-ST07 | Major | Inconsistent exception handling | Unhandled exceptions |
| GAP-ST08 | Major | Physics thread safety not enforced | Data races |
| GAP-ST09 | Major | SimpleConsole partial thread safety | Output corruption |
| GAP-ST10 | Major | NetworkManager potential deadlock | Hang under load |
| GAP-ST11 | Major | Atomic memory ordering unaudited | Potential data races |
| GAP-ST12 | Moderate | No GPU device loss recovery | Crash on driver update |
| GAP-ST13 | Moderate | ObjectPool potential leak | Memory leak |
| GAP-ST14 | Moderate | Module loading minimal validation | Crash on bad DLL |
| GAP-ST15 | Moderate | Binary serialization fragile | Save file corruption |
| GAP-ST16 | Moderate | No memory budget tracking | OOM without warning |
| GAP-ST17 | Minor | No release-build enum validation | Unexpected fallthrough |
| GAP-ST18 | Minor | Static panel variables | Editor data corruption |
| GAP-ST19 | Minor | Signal handler unsafe | Secondary crash |
| GAP-ST20 | Minor | No watchdog timer | Hangs undetected |

---

## Aggregate Statistics

| Metric | Value |
|---|---|
| Total gaps identified | 20 |
| Critical | 6 |
| Major | 5 |
| Moderate | 5 |
| Minor | 4 |
| Raw `new` allocations | 141 across 54 files |
| Raw `delete` calls | 32 across 7 files |
| `reinterpret_cast` usages | 130 across 21 files |
| Global pointer files | 16 |
| Exception catch blocks | 112 (concentrated in 4 files) |
| Files with zero exception handling | ~90% of source files |

---

## Recommended Priority Order

1. **GAP-ST03** — Deferred entity destruction (prevents crashes during gameplay)
2. **GAP-ST06** — Crash handler log flush (enables post-crash diagnosis)
3. **GAP-ST02** — Replace globals with EngineContext (shutdown stability)
4. **GAP-ST01** — Smart pointer migration for Physics (biggest leak source)
5. **GAP-ST08** — Physics thread safety assertions (prevents hard-to-diagnose crashes)
6. **GAP-ST07** — Exception handling at subsystem boundaries
7. **GAP-ST10** — NetworkManager mutex redesign
8. **GAP-ST04** — Dynamic log message buffers
9. **GAP-ST05** — Audit `reinterpret_cast` for strict aliasing
10. Everything else
