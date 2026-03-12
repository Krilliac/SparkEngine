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

## Resource Lifecycle

The D3D12 backend uses deferred deletion to safely release GPU resources:

1. **Creation** — Resources are created on the main thread via `D3D12Device`
2. **Usage** — Resources are referenced in command lists during rendering
3. **Deferred Deletion** — When a resource is no longer needed, it is queued for deletion with the current fence value
4. **Actual Release** — Once the GPU has completed all work up to that fence value, the resource is released

```cpp
// Resources are automatically tracked and deleted when the GPU catches up
device->DeferDelete(resource, currentFenceValue);

// At the end of each frame, completed resources are released
device->ProcessDeferredDeletions();
```

This prevents use-after-free crashes that can occur when the CPU releases a resource the GPU is still using.

## Per-Frame Resources

The backend uses double-buffered command allocators to avoid GPU stalls:

```
Frame N:     [Record Commands] → [Submit] → [GPU Executes]
Frame N+1:   [Record Commands] → [Submit] → [GPU Executes]
                                              ↑
                                    Fence signals completion
```

Each frame has its own:
- Command allocator (reset when the GPU finishes with that frame)
- Dynamic constant buffer region
- Descriptor heap offset for shader-visible resources

## Swap Chain Configuration

The flip-model swap chain supports:

| Feature | Support |
|---------|---------|
| DXGI 1.5+ | Required |
| Flip Discard | Default presentation model |
| Tearing (VRR) | Supported when available |
| HDR Output | Detected via `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` |
| Buffer Count | 2 (double buffering) or 3 (triple buffering) |

## Command Queue Architecture

Three command queues are created for maximum GPU utilization:

| Queue | Type | Usage |
|-------|------|-------|
| **Direct** | `D3D12_COMMAND_LIST_TYPE_DIRECT` | Graphics rendering, main pipeline |
| **Copy** | `D3D12_COMMAND_LIST_TYPE_COPY` | Texture uploads, buffer transfers |
| **Compute** | `D3D12_COMMAND_LIST_TYPE_COMPUTE` | Async compute, post-processing |

Copy and compute queues run concurrently with the direct queue, enabling texture uploads and compute work to overlap with rendering.

## Descriptor Heap Management

The `DescriptorHeapAllocator` manages GPU-visible descriptor heaps using a free-list allocator:

| Heap Type | Capacity | Visibility |
|-----------|----------|------------|
| CBV/SRV/UAV | 1,000,000 | Shader-visible |
| RTV | 256 | CPU-only |
| DSV | 64 | CPU-only |
| Sampler | 2,048 | Shader-visible |

```cpp
// Allocate a range of descriptors
auto allocation = heapAllocator.Allocate(CBV_SRV_UAV, 16);
// Use allocation.cpuHandle and allocation.gpuHandle

// Free when done (deferred until GPU catches up)
heapAllocator.Free(allocation);
```

## Debug and Validation

When enabled in debug builds, the backend activates:

- **D3D12 Debug Layer** — Validates API usage, reports errors
- **GPU-Based Validation** — Catches shader-level errors (expensive, use sparingly)
- **DRED (Device Removed Extended Data)** — Provides detailed crash diagnostics
- **PIX Event Markers** — Named regions for GPU profiling in PIX/RenderDoc

## Integration with RHI

The D3D12 backend integrates with the [RHI abstraction layer](RHI-Abstraction-Layer) through the `IRHIDevice` interface. `D3D12Device` implements all abstract resource creation, command list management, and capability query methods.

## Threading Model

- Resource creation: main thread only
- Command list recording: thread-safe (one list per thread)
- Command submission: serialized via `m_submitMutex`
- Deferred deletion: frame-fenced, processed on main thread
- Descriptor allocation: lock-free within pre-allocated ranges

## Console Commands

```
d3d12_info           # Show D3D12 device info and feature levels
d3d12_heaps          # Show descriptor heap usage
d3d12_memory         # Show GPU memory usage and budget
d3d12_debug <on|off> # Toggle debug layer validation messages
```

## Performance Tips

- **Minimize root signature changes** — The default root signature handles most cases
- **Use copy queue for uploads** — Overlap texture uploads with rendering
- **Batch descriptor writes** — Copy descriptors in bulk rather than one at a time
- **Monitor VRAM budget** — Use `IDXGIAdapter3::QueryVideoMemoryInfo` to stay within budget

---

## See Also

- [RHI Abstraction Layer](RHI-Abstraction-Layer) — Backend-agnostic graphics interface
- [Rendering and Graphics](Rendering-and-Graphics) — Render pipelines and materials
- [DXR Raytracing](DXR-Raytracing) — Ray tracing built on D3D12
- [Upscaling (DLSS/FSR)](Upscaling-System) — Upscaling techniques
- [Shader Pipeline](Shader-Pipeline) — Shader compilation for D3D12
- [Profiler and Debugging](Profiler-and-Debugging) — GPU profiling and debug tools
