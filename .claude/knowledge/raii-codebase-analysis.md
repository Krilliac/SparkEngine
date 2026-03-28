# RAII Codebase Analysis

**Last updated:** 2026-03-28
**Type:** Observation
**Status:** Resolved

## Description

Full deep analysis of RAII (Resource Acquisition Is Initialization) patterns, violations, and recommendations across the entire SparkEngine codebase. Covers smart pointers, ComPtr, resource leaks, scope guards, factory methods, singletons, and cleanup ordering.

## Context

The codebase targets C++23 and mandates `std::unique_ptr` for ownership, raw pointers for non-owning references, `ComPtr` for COM objects, and no naked `new`/`delete`. This audit checks compliance across all subsystems.

---

## 1. Overall RAII Compliance by Subsystem

| Subsystem | Grade | Key Pattern | Notes |
|-----------|-------|-------------|-------|
| Graphics/RHI (D3D12) | A+ | ComPtr + unique_ptr | Deferred deletion queue, fence-based cleanup |
| Graphics/RHI (Vulkan) | A | Value-type handles + RAII destructors | Proper VkDestroy* in destructors |
| Graphics/RHI (D3D11) | B | ComPtr mostly | 2 raw non-owning pointers need documentation |
| Graphics/RHI (OpenGL) | B | Platform handle cleanup in destructors | X11/EGL handles properly ordered |
| Core Engine | A | unique_ptr globals, reverse shutdown | 50+ singletons use manual Init/Shutdown |
| ECS | A | unique_ptr factory returns | EnTT world by reference |
| Editor | A | shared_ptr panel model | Non-owning .get() for dependencies |
| Physics (Jolt) | C+ | Raw `new` for Jolt types | Jolt ref-counting manages lifetime |
| Networking | A- | RAII mutex locks, socket cleanup | No socket RAII wrapper but all paths close |
| Utils | A | ScopeGuard, ScopedTimer, lock_guard | ScopeGuard exists but unused in production |

---

## 2. Existing RAII Infrastructure (Well-Implemented)

### ScopeGuard (`Utils/ScopeGuard.h`)
- Three guard types: `ScopeExit`, `ScopeSuccess`, `ScopeFail`
- Factory helpers: `MakeScopeExit()`, `MakeScopeSuccess()`, `MakeScopeFail()`
- Move-constructible, non-copyable, dismissible
- **CRITICAL: Zero usage in production code** — only used in `Tests/TestScopeGuard.cpp`

### DeferredDeletionQueue (`Graphics/RHI/DeferredDeletionQueue.h`)
- Frame-delayed GPU resource destruction (default: 3 frames)
- Thread-safe (mutex-protected Enqueue/ProcessQueue/FlushAll)
- Prevents use-after-free with in-flight GPU command lists

### ScopedTimer (`Utils/ScopedTimer.h`)
- RAII performance measurement with auto callback on destruction
- Macro: `SPARK_SCOPED_TIMER(name)`

### ComPtr Usage (60+ files in Graphics)
- All D3D11/D3D12 COM interfaces properly wrapped
- D3D12 has additional fence-based deferred release queue

### Mutex RAII (88+ files)
- `std::lock_guard<std::mutex>` used extensively
- `std::unique_lock<std::mutex>` for condition variables
- **Zero raw mutex.lock()/unlock() calls found** — all RAII

### Smart Pointers (380+ instances)
- `std::unique_ptr` dominant for ownership
- `std::shared_ptr` for shared resources (textures, lights, render targets)
- `std::make_unique`/`std::make_shared` used correctly throughout
- 30+ classes properly delete copy constructors

---

## 3. Violations and Issues Found

### HIGH PRIORITY

#### H1: ScopeGuard Never Used in Production
- **Impact**: All the infrastructure exists but no production code uses it
- **Location**: `Utils/ScopeGuard.h` (definitions), only `Tests/TestScopeGuard.cpp` (usage)
- **Risk**: FILE*, HANDLE, and COM cleanup not exception-safe

#### H2: Physics Layer — Extensive Raw `new` (15+ instances)
- **Location**: `Physics/PhysicsShapeFactory.cpp` (lines 110-357), `Physics/VehiclePhysics.cpp` (lines 65-177), `Physics/CharacterController.cpp` (line 55-60), `Physics/SoftBodyPhysics.cpp` (line 45), `Physics/PhysicsSystemQueries.cpp` (lines 716, 1405)
- **Pattern**: `new JPH::ShapeRefC(result.Get())`, `new JPH::WheelSettingsWV`, etc.
- **Mitigation**: Jolt uses internal ref-counting (`JPH::Ref<T>`, `JPH::RefConst<T>`) so these are typically transferred into Jolt-owned containers. Not trivially convertible to unique_ptr without Jolt API changes.
- **Risk**: Medium — relies on Jolt's internal ref-counting being correct

#### H3: Physics — Manual `delete` (2 instances)
- **Location**: `Physics/PhysicsSystem.cpp:460` — `delete JPH::Factory::sInstance`
- **Location**: `Physics/CharacterController.cpp:87` — `delete m_joltCharacter`
- **Risk**: Not exception-safe; `m_joltCharacter` should be unique_ptr

#### H4: Mesh.cpp — Raw COM Release (4 calls)
- **Location**: `Graphics/Mesh.cpp:49,54,454,459` — `m_ib->Release()` / `m_vb->Release()`
- **Issue**: `ID3D11Buffer*` members should be `ComPtr<ID3D11Buffer>`
- **Risk**: If Shutdown() not called or exception thrown, COM objects leak

#### H5: CrashHandlerHelpers.cpp — Manual COM Release Chain
- **Location**: `Utils/CrashHandlerHelpers.cpp` (lines 835-919)
- **Issue**: 10+ manual `->Release()` calls in branching paths (backBuffer, staging, wicFactory, wicBitmap, wicStream, etc.)
- **Risk**: Multiple early return paths could skip Release() calls. Should use ComPtr<>.
- **Mitigation**: Crash handler runs once at crash time, so practical risk is low

### MEDIUM PRIORITY

#### M1: D3D11 Non-Owning Raw Pointers (undocumented)
- **Location**: `Graphics/RHI/D3D11/D3D11Device.h:211` — `ID3D11Device* m_device` in D3D11SwapChain
- **Location**: `Graphics/RHI/D3D11/D3D11Device.h:267` — `ID3D11DeviceContext* m_context` in D3D11CommandList
- **Issue**: Non-owning references with no documentation; if parent D3D11Device destroyed first, dangling pointer
- **Fix**: Add `// Non-owning; lifetime tied to parent D3D11Device` comment, or use ComPtr<> to add ref

#### M2: D3D11PipelineState Raw Shader Pointers
- **Location**: `Graphics/RHI/D3D11/D3D11Device.h:176-177` — `D3D11Shader* m_vertexShader`, `D3D11Shader* m_pixelShader`
- **Issue**: Raw pointers to shaders managed elsewhere; dangling pointer risk if shaders destroyed before pipeline state

#### M3: RenderTarget.cpp — FILE* Without RAII
- **Location**: `Graphics/RenderTarget.cpp:350` — `FILE* file = fopen(...)`
- **Issue**: If exception thrown between fopen/fclose, file handle leaks
- **Fix**: Use `auto guard = Spark::MakeScopeExit([&]{ if (file) fclose(file); });`

#### M4: ConsoleProcessManagerWin32.cpp — Multiple CloseHandle Calls
- **Location**: `Utils/ConsoleProcessManagerWin32.cpp` (lines 110-144)
- **Issue**: 6 manual CloseHandle() calls for pipe/process/thread handles
- **Fix**: RAII handle wrapper class

#### M5: Singleton Manual Lifecycle (50+ instances)
- **Location**: `Core/SparkEngine.cpp` (lines 239-662)
- **Pattern**: All singletons require explicit `.Initialize()` and `.Shutdown()` calls
- **Risk**: Forgetting `.Shutdown()` leaks resources. Shutdown order must match reverse init order.
- **Current state**: Init/shutdown organized into groups (AI, Rendering, Gameplay, Debug) but not enforced programmatically

### LOW PRIORITY

#### L1: Shader.cpp — Verbose unique_ptr Construction
- **Location**: `Graphics/Shader.cpp:130-131`
- **Pattern**: `std::unique_ptr<T>(new T())` instead of `std::make_unique<T>()`
- **Fix**: Trivial modernization

#### L2: shared_ptr Over-Use
- Some single-ownership scenarios use `std::shared_ptr` where `std::unique_ptr` suffices
- Examples: Some entries in `m_lights` containers, some render targets
- **Risk**: None (performance overhead negligible)

#### L3: CoInitializeEx Without RAII Guard
- **Location**: `Utils/CrashHandlerHelpers.cpp:862`
- **Pattern**: Manual CoInitializeEx/CoUninitialize pairing
- **Fix**: Use COM initialization guard class

---

## 4. Factory Method Compliance

All RHI factory methods return `std::unique_ptr` — **excellent compliance**:
- `RHI::CreateDevice()` → `std::unique_ptr<IRHIDevice>`
- `RHIBridge::CreateVertexBuffer()` → `std::unique_ptr<IRHIBuffer>`
- `RHIBridge::CreateIndexBuffer()` → `std::unique_ptr<IRHIBuffer>`
- `RHIBridge::CreateConstantBuffer()` → `std::unique_ptr<IRHIBuffer>`
- `RHIBridge::CreateTexture2D()` → `std::unique_ptr<IRHITexture>`
- `RHIBridge::CreateDepthBuffer()` → `std::unique_ptr<IRHITexture>`
- `RHIBridge::CreateRenderTarget()` → `std::unique_ptr<IRHITexture>`
- `RHIBridge::CreateSamplerLinearWrap()` → `std::unique_ptr<IRHISampler>`
- All D3D11/D3D12/Vulkan/OpenGL backends consistently return unique_ptr
- ECS `CreateReactiveSystems()` returns `std::vector<std::unique_ptr<ISystem>>`
- Editor panels use `std::make_shared<>()` registration pattern

---

## 5. Engine Shutdown Order

Shutdown is organized in reverse initialization order:
1. `ShutdownGameplaySystems()` — Gameplay singletons
2. `ShutdownDebugSystems()` — Debug/profiling singletons
3. `ConsoleProcessManager::Shutdown()` — IPC pipes
4. `ModuleManager::ShutdownAll()` + `UnloadAll()` — Game DLLs
5. `audioEngine.reset()` — Audio
6. `ShutdownPhysics()` — Jolt cleanup + `g_physicsOwned.reset()`
7. Implicit unique_ptr destruction for Timer, EventBus, etc.

---

## 6. Recommendations (Priority Order)

1. **Start using ScopeGuard in production** — it exists, it's tested, it's unused
2. **Wrap Mesh.cpp buffers in ComPtr<>** — eliminate 4 manual Release() calls
3. **Add non-owning pointer comments** in D3D11SwapChain/D3D11CommandList
4. **Use unique_ptr for CharacterController::m_joltCharacter**
5. **Wrap FILE* in ScopeExit guard** in RenderTarget::SaveToFile
6. **Replace CrashHandler COM manual Release chain with ComPtr<>**
7. **Consider RAII handle wrapper** for Windows HANDLE in ConsoleProcessManager

---

## Notes

- Vulkan handles are value types (not pointers) — no RAII wrapper needed, just destroy in destructor
- Jolt Physics `new` usage is pervasive but by design — Jolt's ref-counting system manages lifetime
- The codebase has zero raw mutex.lock()/unlock() calls — all mutex access uses RAII guards
- D3D12 backend is the gold standard for RAII compliance in this codebase
- 30+ classes properly delete copy constructors/assignment operators
