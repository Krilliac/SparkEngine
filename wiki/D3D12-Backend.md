# D3D12 Backend

## Overview

SparkEngine's D3D12 backend provides a modern, low-level graphics API implementation using Direct3D 12. It sits behind the RHI (Rendering Hardware Interface) abstraction layer.

## Architecture

- **Namespace:** `Spark::RHI::D3D12`
- **Files:** `Graphics/RHI/D3D12/D3D12Device.h` (773 lines), `D3D12Device.cpp` (1507 lines)
- **Guard:** `#ifdef _WIN32`

## Key Classes

| Class | Purpose |
|-------|---------|
| `D3D12Device` | Main device — implements `IRHIDevice` |
| `D3D12CommandList` | Command recording — implements `IRHICommandList` |
| `D3D12SwapChain` | DXGI swap chain — implements `IRHISwapChain` |
| `D3D12Buffer/Texture/Shader/Sampler/PipelineState` | GPU resources |
| `DescriptorHeapAllocator` | Free-list descriptor heap management |
| `D3D12Fence` | RAII CPU/GPU synchronization |

## Features

- **Debug Layer:** Optional validation with GPU-based validation support
- **3 Command Queues:** Direct (graphics), Copy, Compute
- **4 Descriptor Heaps:** CBV/SRV/UAV (1M), RTV (256), DSV (64), Sampler (2048)
- **Flip-Model Swap Chain:** DXGI 1.5+ with `FLIP_DISCARD` and tearing support
- **Deferred Deletion:** Resources queued with fence values, released when GPU completes
- **Per-Frame Resources:** Double-buffered command allocators with fence sync
- **DXR Detection:** Queries `ID3D12Device5` and raytracing tier
- **Mesh Shader Detection:** Queries `D3D12_OPTIONS7`
- **Bindless Resources:** Detects `RESOURCE_BINDING_TIER_3`

## Capability Detection

```cpp
auto& caps = device->GetCapabilities();
caps.rayTracingSupport;          // DXR 1.0+
caps.meshShaderSupport;          // Mesh shader tier
caps.bindlessResourceSupport;    // Tier 3 binding
caps.conservativeRasterSupport;  // Conservative raster
```

## Root Signature Layout

The default root signature provides:
- **Param 0:** CBV table (b0-b13, all stages)
- **Param 1:** SRV table (t0-t31, pixel shader)
- **Param 2:** Sampler table (s0-s15, pixel shader)
- **Param 3:** UAV table (u0-u7, all stages)

## RHI Factory Registration

```cpp
// In RHIFactory.cpp
case GraphicsBackend::D3D12:
    device = std::make_unique<D3D12::D3D12Device>();
    break;
```

## Threading Model

- Resource creation: main thread only
- Command list recording: thread-safe (one list per thread)
- Command submission: serialized via `m_submitMutex`
