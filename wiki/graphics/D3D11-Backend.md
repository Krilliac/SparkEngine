# D3D11 Backend

DirectX 11 is SparkEngine's primary Windows implementation and the renderer candidate in the blocked, uncertified `stable-v1` Windows 11 x64 profile. Auto-selection prefers it on Windows when compiled in. The implementation contains the major rendering paths described below, but their stable-v1 certification gates remain open; this page does not claim production or known-stable status.

## Overview

- **Namespace:** `Spark::RHI::D3D11`
- **Files:** `Graphics/RHI/D3D11/D3D11Device.{h,cpp}` (~360 LOC header)
- **Guard:** `#ifdef _WIN32` — compiled only on Windows builds.
- **Links against:** `d3d11.dll`, `dxgi.dll`, `d3dcompiler_47.dll`, `dxguid.lib` (automatic via MSVC `#pragma comment`).
- **Feature level:** 11_0 (minimum), 11_1 (preferred).

## Architecture

| Class | Purpose | RHI interface |
|-------|---------|---------------|
| `D3D11Device` | Adapter + device + immediate context owner | `IRHIDevice` |
| `D3D11Buffer` | GPU buffer backed by `ID3D11Buffer` | `IRHIBuffer` |
| `D3D11Texture` | 1D/2D/3D/Cube texture + SRV/RTV/DSV views | `IRHITexture` |
| `D3D11Shader` | Any shader stage + compiled bytecode | `IRHIShader` |
| `D3D11Sampler` | Sampler state | `IRHISampler` |
| `D3D11PipelineState` | Aggregate rasterizer / depth / blend / VS+PS | `IRHIPipelineState` |
| `D3D11SwapChain` | DXGI 1.2+ flip-model swap chain | `IRHISwapChain` |
| `D3D11CommandList` | Immediate-mode translation of deferred RHI calls | `IRHICommandList` |

Resources use `Microsoft::WRL::ComPtr` (RAII for COM objects). No raw `AddRef` / `Release` calls in user code.

### Transient bump allocator

Each `D3D11Device` owns a `TransientBufferAllocator` used for per-frame CPU-visible vertex/index data — debug draw, particle batches, and UI geometry all use it instead of allocating fresh buffers every frame. The allocator resets on swap-chain present.

## Features

- **Debug layer** — auto-enabled on Debug builds (`D3D11_CREATE_DEVICE_DEBUG` + info-queue breakpoints).
- **WARP fallback** — if no hardware adapter matches, the engine retries with `D3D_DRIVER_TYPE_WARP`; see [RHI Abstraction Layer](RHI-Abstraction-Layer.md#nullrhidevice-selection-and-bridge-failover) for the broader fallback chain.
- **Deferred deletion queue** — resources freed mid-frame are kept alive until the GPU has finished referencing them (see `DeferredDeletionQueue.h`).
- **Tearing / flip-discard** — swap chain opts into `DXGI_SWAP_EFFECT_FLIP_DISCARD` and `DXGI_FEATURE_PRESENT_ALLOW_TEARING` when the adapter reports support.
- **MSAA / sRGB / HDR formats** — supported; per-texture MSAA sample count is validated via `CheckMultisampleQualityLevels`.

## Adapter / capability detection

`D3D11Device::Initialize` calls `IDXGIFactory1::EnumAdapters1`, picks the one with the largest dedicated video memory, and records:

```cpp
caps.maxTextureSize         // 16384 on FL11, 2048 on FL10
caps.computeShaderSupport   // true on FL10+
caps.tessellationSupport    // true on FL11+
caps.maxMSAASamples         // 1/2/4/8 based on format checks
```

These land on `IRHIDevice::GetCapabilities()` and feed the engine's feature gates (e.g. TAA disables itself if MSAA > 1 is forced elsewhere).

## Shader compilation

Shaders compile through `D3DCompile` (`d3dcompiler_47.dll`). Entry points: `vs_5_0`, `ps_5_0`, `cs_5_0`, etc. For the offline shader cook path, see [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md).

## Factory registration

```cpp
// RHIFactory.cpp
case GraphicsBackend::D3D11:
    device = std::make_unique<D3D11::D3D11Device>();
    break;
```

Selected automatically by `RHIFactory::GetRecommendedBackend()` on Windows.

## Limitations

- **No bindless** — binding model is CBV / SRV / UAV slots. Use [D3D12](D3D12-Backend.md) if your workload needs descriptor-heap bindless.
- **No mesh shaders** — requires D3D12 + SM 6.5.
- **No DXR** — ray tracing requires D3D12. [Hybrid Ray Tracing](Hybrid-Ray-Tracing.md) falls back to screen-space techniques on D3D11.
- **No timeline semaphores** — synchronization is fence-based.

## Related

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md) — common interface every backend implements.
- [D3D12 Backend](D3D12-Backend.md) — modern low-level alternative on Windows.
- [Vulkan Backend](Vulkan-Backend.md) — cross-platform equivalent.
- [Render Graph](Render-Graph.md) — declarative pipeline that runs on top of any backend.
