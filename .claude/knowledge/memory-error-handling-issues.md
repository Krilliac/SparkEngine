# Memory Safety and Error Handling Issues

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Active
**Severity:** High

## Description

Naked new/delete in Physics and RHI, unchecked HRESULT returns in D3D11, integer underflow risks, division by zero, and const-correctness violations.

---

## Memory Safety

### Naked new/delete in PhysicsSystem

**File:** `Physics/PhysicsSystem.cpp:726-734`

```cpp
m_collisionConfig = new btDefaultCollisionConfiguration();
m_dispatcher = new btCollisionDispatcher(m_collisionConfig);
m_broadphase = new btDbvtBroadphase();
m_solver = new btSequentialImpulseConstraintSolver();
m_dynamicsWorld = new btDiscreteDynamicsWorld(...);
```

Raw owning pointers declared in `PhysicsSystem.h:1720-1732`. Properly deleted in Shutdown() but not exception-safe.

**Fix:** Use `std::unique_ptr<btFoo>` with custom deleters if needed.

### RHI .release() Breaking Ownership

**File:** `Graphics/RHI/D3D11/D3D11Device.cpp:619,676,792,814,913,990`

```cpp
return std::make_unique<D3D11Buffer>(desc, std::move(buffer)).release();
```

Creates unique_ptr then immediately releases it, returning raw pointer. Callers must manually delete via `Destroy*()` methods.

**Fix:** Return `std::unique_ptr<T>` from Create*() methods.

### COM Manual Release in TextureSystem

**File:** `Graphics/TextureSystem.cpp:120-127`

Manual `pFactory->Release()` calls instead of ComPtr. Not exception-safe.

---

## Error Handling

### Unchecked HRESULT (15+ instances)

**D3D11Device.cpp** — Lines 155, 657, 663, 672, 720:
- `CreateShaderResourceView` — no check
- `CreateRenderTargetView` — no check
- `CreateDepthStencilView` — no check
- `QueryInterface` — no check (null dereference if fails)
- `D3DCreateBlob` — no check (crash on next line if fails)

**D3D12Device.cpp** — Line 973: `D3DCreateBlob` unchecked.

**DeferredLightingPass.h** — Lines 245-270: 7 unchecked device creation calls.

**ForwardPlusLightCulling.h** — Lines 242-300: 6 unchecked device creation calls.

### Integer Underflow: `size() - 1` on Empty Containers

| File | Line | Expression |
|------|------|-----------|
| `Engine/Replay/ReplaySystem.cpp` | 358 | `frames.size() - 1` (wraps to SIZE_MAX if empty) |
| `Engine/Animation/AnimationCompression.cpp` | 203, 235, 567, 597 | `track.times.size() - 1` |
| `Engine/Loading/LoadingScreen.cpp` | 134 | `size() - 1` |
| `Graphics/LightingSystem.cpp` | 679, 2016 | `size() - 1` |

**Fix:** Add `if (container.empty()) return;` guard before arithmetic.

### Division by Zero Risks

| File | Line | Expression |
|------|------|-----------|
| `Graphics/WeatherSystem.h` | 272 | `dt / m_transitionDuration` |
| `Input/GamepadInput.cpp` | 355 | `normalized / magnitude` |
| `Physics/ClothSimulation.cpp` | 34 | `mass / (width * height)` |
| `Physics/PhysicsSystem.cpp` | 500 | Float equality check on zero |

### Silent Catch Blocks

**File:** `Utils/SparkConsole.cpp:262-293,989-1004`

Two `catch(...)` blocks that swallow all exceptions silently during config save and history save.

### Asserts on Runtime Conditions

**File:** `Graphics/TextureSystem.cpp:37,134`

`ASSERT(device)` and `ASSERT(device && data && dataSize > 0)` — stripped in Release builds. Should be runtime checks with error returns.

### Unchecked std::stof in Config Loading

**File:** `Utils/SparkConsole.cpp:326-346`

10+ `std::stof(value)` calls without try-catch. Non-numeric config values crash.

---

## Const-Correctness Violations (70+ instances)

CLAUDE.md: *"const on all non-mutating methods and parameters"*

**Worst offenders:**
- `Graphics/PostProcessingPipeline.h` — 9 GetSettings() methods not marked const
- `Graphics/GraphicsIntegration.h:162-166` — 5 Get* methods not const
- `Graphics/WaterSystem.h:845` — GetSettings() not const
- `Graphics/RenderGraph.h:931` — GetBlackboard() not const
- `Graphics/RenderTarget.h:146` — GetDesc() not const

70+ total getter methods across Graphics subsystem missing const qualifier.

---

## Mismatched Header/CPP First Includes (15 files)

These .cpp files don't include their own .h as the first include:

SparkConsole.cpp, ConsoleProcessManager.cpp, D3DUtils.cpp, CrashHandler.cpp, SplinePath.cpp, FileUtils.cpp, MathUtils.cpp, SplineMath.cpp, RenderTarget.cpp, RenderPipeline.cpp, GraphicsConsoleCommands.cpp, TextureSystem.cpp, AssetPipeline.cpp, D3D11Device.cpp, AssetDatabase.cpp

All include `Core/Platform.h` or `<algorithm>` first instead.

---

## Plain enum (Not enum class) — 4 Instances

**File:** `Core/Platform.h:1335-1367`

4 unscoped enums: `DXGI_FORMAT`, `D3D11_BIND_FLAG`, `D3D11_FILTER`, `D3D11_TEXTURE_ADDRESS_MODE`

These are D3D11 API stubs for Linux cross-compilation. Converting to `enum class` would break API compatibility. Acceptable exception.
