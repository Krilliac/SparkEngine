# Memory Safety and Error Handling Issues

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Mostly Resolved
**Severity:** High

## Description

Naked new/delete in Physics and RHI, unchecked HRESULT returns in D3D11, integer underflow risks, division by zero, and const-correctness violations. Most issues have been resolved.

---

## Memory Safety

### Naked new/delete in PhysicsSystem — OPEN (low risk)

**File:** `Physics/PhysicsSystem.cpp:726-734`

Raw owning pointers for Bullet Physics objects. Properly deleted in Shutdown() but not exception-safe.

**Note:** Converting to `std::unique_ptr<btFoo>` would require custom deleters and Bullet header changes. Low priority since Shutdown() correctly cleans up.

### RHI .release() Breaking Ownership — OPEN (architectural)

**File:** `Graphics/RHI/D3D11/D3D11Device.cpp`

`std::make_unique<T>().release()` pattern returns raw pointers. This is the RHI's interface design — callers use `Destroy*()` methods. Changing to `std::unique_ptr` return types would require RHI API changes across all backends.

### COM Manual Release in TextureSystem — OPEN (low risk)

**File:** `Graphics/TextureSystem.cpp:120-127`

Manual `pFactory->Release()` calls instead of ComPtr. Windows-only code path.

---

## Error Handling

### Unchecked HRESULT — RESOLVED

All D3D11/D3D12 HRESULT calls now check return values. Fixed in prior sessions.

### Integer Underflow: `size() - 1` — RESOLVED

All instances verified safe — either guarded by empty checks or occur after push_back.

### Division by Zero — RESOLVED

All 4 instances fixed: WeatherSystem.h (ternary guard), GamepadInput.cpp (deadzone check), ClothSimulation.cpp (early return), PhysicsSystem.cpp (already safe).

### TextureSystem ASSERT on Runtime Conditions — RESOLVED

**File:** `Graphics/TextureSystem.cpp:37,134`

**Fix applied:** Replaced `ASSERT(device)` and `ASSERT(device && data && dataSize > 0)` with `if (!...) return E_INVALIDARG;` — proper runtime checks that work in Release builds.

### Silent Catch Blocks — OPEN (low risk)

**File:** `Utils/SparkConsole.cpp`

Two `catch(...)` blocks swallow exceptions during config/history save. Low impact — failure to save console history is non-critical.

---

## Const-Correctness — RESOLVED

All reported getter methods in Graphics headers already have proper const overloads:
- PostProcessingPipeline.h — all 9 GetSettings() methods have const/non-const pairs
- RenderGraph.h — GetBlackboard() has both overloads
- RenderTarget.h — GetDesc() has both overloads
- GraphicsIntegration.h — file deleted (orphaned)

---

## Mismatched Header/CPP First Includes (15 files) — OPEN (cosmetic)

Low priority. Affects compilation order but not correctness.

---

## Plain enum (Not enum class) — 4 Instances — ACCEPTABLE

D3D11 API stubs in `Core/Platform.h` for Linux cross-compilation. Converting to `enum class` would break API compatibility.
