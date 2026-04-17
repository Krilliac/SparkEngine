# Metal Backend

**Experimental** macOS backend using Apple's Metal API directly (not via MoltenVK). The implementation is header-designed today — the `.mm` implementation file is the active piece of work on the Tier-1 roadmap; see the [Feature Roadmap](../../docs/plans/FEATURE_ROADMAP.md).

## Overview

- **Namespace:** `Spark::RHI::Metal`
- **Header:** `Graphics/RHI/Metal/MetalDevice.h` (~540 LOC — full RHI interface implementation, wrapping Objective-C Metal types).
- **Guard:** `#ifdef __APPLE__ && ENABLE_METAL`
- **Language:** C++/Objective-C++ (`.mm`). The header is pure C++ with ARC-bridged handles.
- **Minimum target:** macOS 12 (Monterey) — the minimum Apple still ships `MTLDevice` with compute + ray-tracing capabilities.

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
    device = std::make_unique<Metal::MetalDevice>();
    break;
```

Selected by `RHIFactory::GetRecommendedBackend()` on macOS *when the Metal `.mm` implementation is compiled in*. Until that ships, the macOS recommendation falls back to the Vulkan backend (via MoltenVK) or NullRHIDevice.

## Current status

- **Stable:** header interface, `MetalBuffer` / `MetalTexture` / `MetalShader` / `MetalSampler` ABI, capability queries.
- **Experimental:** the `.mm` implementation and pipeline state plumbing.
- **Planned:** full ray-tracing pipeline integration, argument buffers, tile shaders, MetalFX.

See [PROJECT_STATUS.md](../../docs/status/PROJECT_STATUS.md) for the authoritative maturity table.

## Related

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md)
- [Vulkan Backend](Vulkan-Backend.md) — interim fallback on macOS via MoltenVK.
- [Upscaling System](Upscaling-System.md) — MetalFX integration target.
