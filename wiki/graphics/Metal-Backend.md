# Metal Backend

> **Release boundary:** This page documents an experimental implementation outside
> the blocked and uncertified `stable-v1` product scope (Windows 11 x64/MSVC
> v143 with D3D11 or Windows NullRHI and C++ modules). It is not a supported or
> release-certified backend.

**Experimental** macOS backend using Apple's Metal API directly (not via MoltenVK). Current source contains Objective-C++ device, interop, and texture-readback implementations; it remains partial, blocked, and outside `stable-v1`.

## Overview

- **Namespace:** `Spark::RHI::Metal`
- **Source:** `Graphics/RHI/Metal/MetalDevice.{h,mm}`, `MetalInterop.{h,mm}`, and `MetalTextureReadback.{h,mm}`.
- **Guard:** `#ifdef __APPLE__ && ENABLE_METAL`
- **Language:** C++/Objective-C++ (`.mm`). The header is pure C++ with ARC-bridged handles.
- **Host status:** the macOS source/build path is experimental; no qualified OS/hardware support matrix or release certification exists.

## Architecture

| Class | Purpose | RHI interface |
|-------|---------|---------------|
| `MetalDevice` | `id<MTLDevice>` owner + command queue | `IRHIDevice` |
| `MetalBuffer` | `id<MTLBuffer>` — shared / managed / private storage | `IRHIBuffer` |
| `MetalTexture` | `id<MTLTexture>` with optional `MTLPixelFormat` reinterpretation | `IRHITexture` |
| `MetalShader` | `id<MTLFunction>` from `MTLLibrary` | `IRHIShader` |
| `MetalSampler` | `id<MTLSamplerState>` | `IRHISampler` |
| `MetalPipelineState` | `id<MTLRenderPipelineState>` / `MTLComputePipelineState` | `IRHIPipelineState` |
| `MetalSwapChain` | `CAMetalLayer` + `MTKView` for presentation | `IRHISwapChain` |
| `MetalCommandList` | `id<MTLCommandBuffer>` + `MTLRenderCommandEncoder` | `IRHICommandList` |

### ObjC handle wrapper

The header defines `ObjCHandle<T>` — an RAII wrapper around `id<T>` that replaces Apple's `__strong` / `__weak` with explicit `Reset()` semantics. This keeps the C++ side clean of Objective-C retain/release details while still participating in ARC.

## Planned features

- **Argument buffers** — Metal's equivalent of descriptor tables / bindless.
- **Tile shaders** — tile-based deferred rendering hooks (Apple silicon).
- **MetalFX upscaling** — integrates with [Upscaling System](Upscaling-System.md) once the base backend is live.
- **MSL codegen** — shader pipeline extension to emit MSL from HLSL via `dxc` + `SPIRV-Cross`.

## Factory registration

```cpp
// RHIFactory.cpp
case GraphicsBackend::Metal:
    device = Metal::CreateDevice();
    break;
```

When compiled, the factory can select Metal and call `Metal::CreateDevice()`. That construction path is implementation evidence only; it does not establish a macOS fallback, parity, support, or `stable-v1` certification. Runtime retries are owned by `RHIBridge`.

## Current status

- **Implemented, experimental:** Objective-C++ device, interop, and synchronous texture-readback paths.
- **Blocked:** backend/shader/pass parity, resource-lifetime, golden-scene, performance, driver, and packaged-macOS proof.
- **Planned:** full ray-tracing pipeline integration, argument buffers, tile shaders, MetalFX.

See [PROJECT_STATUS.md](../../docs/status/PROJECT_STATUS.md) for the authoritative maturity table.

## Related

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md)
- [Vulkan Backend](Vulkan-Backend.md) — experimental development path via MoltenVK.
- [Upscaling System](Upscaling-System.md) — MetalFX integration target.
