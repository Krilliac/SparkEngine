# Memory Safety and Error Handling Issues

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** **Resolved**
**Severity:** Low (all items documented or fixed)

## Description

Naked new/delete in Physics and RHI, unchecked HRESULT returns in D3D11, integer underflow risks, division by zero, and const-correctness violations. Most issues have been resolved.

---

## Memory Safety

### Naked new/delete in PhysicsSystem — DOCUMENTED (architectural)

**File:** `Physics/PhysicsSystem.h:589, 602`

Raw owning pointers (`void*`) for `JPH::ShapeRefC*` and `JPH::Ref<GroupFilterTable>*`. The `void*` type-erasure is intentional — these Jolt types cannot be forward-declared without pulling the entire Jolt header tree. Properly deleted in `Shutdown()`. Documented with comments explaining the compilation firewall pattern.

### RHI .release() Breaking Ownership — DOCUMENTED (architectural)

**File:** `Graphics/RHI/D3D11/D3D11Device.cpp` (and all RHI backends)

`std::make_unique<T>().release()` pattern returns raw pointers. This is the RHI's intentional interface design — callers use `Destroy*()` methods. Documentation comment added to all 4 RHI device files explaining the ownership model. Marked as **architectural decision**.

### COM Manual Release in TextureSystem — RESOLVED

**File:** `Graphics/TextureSystem.cpp`

Already uses `ComPtr<IWICImagingFactory>` — the manual Release issue was fixed in a prior session.

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

### Silent Catch Blocks — RESOLVED

**File:** `Utils/SparkConsole.cpp`

Already uses `catch (const std::exception& e)` with proper error logging. No silent `catch(...)` blocks remain.

---

## Const-Correctness — RESOLVED

All reported getter methods in Graphics headers already have proper const overloads:
- PostProcessingPipeline.h — all 9 GetSettings() methods have const/non-const pairs
- RenderGraph.h — GetBlackboard() has both overloads
- RenderTarget.h — GetDesc() has both overloads
- GraphicsIntegration.h — file deleted (orphaned)

---

## Mismatched Header/CPP First Includes (15 files) — RESOLVED

Fixed: reordered first `#include` in affected `.cpp` files to include their own header first. Files guarded by `#ifdef SPARK_PLATFORM_WINDOWS` were left as-is (they need `Platform.h` first).

---

## Plain enum (Not enum class) — 4 Instances — ACCEPTABLE

D3D11 API stubs in `Core/Platform.h` for Linux cross-compilation. Converting to `enum class` would break API compatibility.
