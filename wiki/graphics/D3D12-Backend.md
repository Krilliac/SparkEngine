# D3D12 Backend

> **Release boundary:** This page documents an experimental implementation outside
> the blocked and uncertified `stable-v1` product scope (Windows 11 x64/MSVC
> v143 with D3D11 or Windows NullRHI and C++ modules). It is not a supported or
> release-certified backend.

## Overview

SparkEngine's D3D12 backend provides a modern, low-level graphics API implementation using Direct3D 12. It sits behind the RHI (Rendering Hardware Interface) abstraction layer.

## Architecture

- **Namespace:** `Spark::RHI::D3D12`
- **Files:** `Graphics/RHI/D3D12/D3D12Device.h`, `D3D12Device.cpp`
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

`RHIDeviceDesc::enableDebugLayer` turns on the D3D12 debug layer (and
`enableGPUValidation` adds GPU-based validation). A requested debug layer that
is not installed (no `d3d12SDKLayers.dll`, the Windows "Graphics Tools"
optional feature) logs a warning and the device runs without validation.

With the layer active, `D3D12Device`:

- stores warning, error and corruption messages in its `ID3D12InfoQueue`
  (a storage filter drops INFO/MESSAGE chatter so the queue limit is spent on
  real findings);
- breaks on error and corruption **only when a debugger is attached**. An
  unconditional break raised a breakpoint exception that killed any process
  without a debugger, so a validation run could not count what it found;
- exposes `GetValidationCounts()` (`active`, `corruption`, `errors`,
  `warnings`, `discarded`). A non-zero `discarded` means warnings or worse were
  lost to the queue limit, so the other counts are a floor, not a total;
- logs the totals at `Shutdown()`: an error line when any error, corruption or
  discard occurred, otherwise "clean".

**Validation evidence (RHI-225).** CTest `D3D12_Validation` (Windows MSVC,
`D3D12_Validation_*`, exact count 3, labels `d3d12;d3d12-validation`) runs
`Tests/TestRHI225D3D12ValidationReal.cpp` with the debug layer and GPU-based
validation on whichever adapter `D3D12Device` selects (hardware, or WARP on a
GPU-less host):

| Test | Declared scope |
|------|----------------|
| `D3D12_Validation_TriangleFrameIsClean` | HLSL VS/PS compiled through `CreateShader`, PSO without depth, dynamic vertex buffer, render-target transitions, clear, draw and a READBACK copy, recorded twice into the reset immediate list; the centre pixel must be the triangle colour, so an empty frame cannot pass |
| `D3D12_Validation_ResourceChurnIsClean` | 200 frames that create dynamic and static buffers and a render-target texture, use the texture on the GPU and destroy all three while that work is in flight (fence-deferred release) |
| `D3D12_Validation_CounterSeesInjectedError` | Negative control: an invalid `CreateCommittedResource` through the native device raises the error count and the process survives |

Each clean test requires zero corruption, zero errors and zero discarded
messages, and fails if the info queue is not active. This is RHI-level
evidence for the listed operations only. Engine frames are not covered:
`GraphicsEngine` renders through D3D11 directly on Windows.

The lane found and fixed two defects on the way: `ConvertFormat` mapped
`PixelFormat::Unknown` to RGBA8, so a PSO without depth got an invalid DSV
format, and `D3D12CommandList::Begin()` kept the previous recording's PSO and
root signature cached, so re-recording the same pipeline skipped its bind on a
freshly reset list. The immediate command list owns a single allocator: it must
be idle (for example after `WaitForIdle()`) before its next `Begin()`.

Other tooling:

- **DRED (Device Removed Extended Data)** — Provides detailed crash diagnostics
- **PIX Event Markers** — `BeginEvent`/`EndEvent`/`SetMarker` are currently no-ops

## Integration with RHI

The D3D12 backend exposes resource, command-list, and capability-query paths through the [RHI abstraction layer](RHI-Abstraction-Layer.md) and `IRHIDevice`. That interface coverage is implementation evidence only: pass parity, synchronization, shader tooling, golden-scene, performance, and driver evidence remain incomplete.

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

## Device Initialization Walkthrough

The `D3D12Device::Initialize()` method follows a strict sequence to set up the D3D12 runtime:

### Step 1: Enable Debug Layer (Debug Builds)

```cpp
// In debug builds, enable the D3D12 debug layer before device creation
#if defined(_DEBUG)
ComPtr<ID3D12Debug> debugInterface;
if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface))))
{
    debugInterface->EnableDebugLayer();

    // Optional: GPU-based validation (expensive but thorough)
    ComPtr<ID3D12Debug1> debugInterface1;
    if (SUCCEEDED(debugInterface.As(&debugInterface1)))
    {
        debugInterface1->SetEnableGPUBasedValidation(TRUE);
    }
}
#endif
```

### Step 2: Create DXGI Factory and Enumerate Adapters

```cpp
// D3D12Device uses IDXGIFactory6 for GPU preference selection
ComPtr<IDXGIFactory6> dxgiFactory;
CreateDXGIFactory2(debugFlags, IID_PPV_ARGS(&dxgiFactory));

// Enumerate adapters, preferring high-performance (discrete GPU)
ComPtr<IDXGIAdapter1> adapter;
dxgiFactory->EnumAdapterByGpuPreference(
    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

// Fallback: WARP software adapter when no hardware GPU is available
if (!adapter || desc.forceSoftware)
{
    dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    m_isSoftwareDevice = true;
}
```

### Step 3: Create Device

```cpp
// Create the D3D12 device with feature level 11_0 as the minimum
HRESULT hr = D3D12CreateDevice(
    adapter.Get(),
    D3D_FEATURE_LEVEL_11_0,
    IID_PPV_ARGS(&m_device)
);

// Query for ID3D12Device5 (needed for DXR)
m_device->QueryInterface(IID_PPV_ARGS(&m_dxrDevice));
```

### Step 4: Create Command Queues, Descriptor Heaps, Frame Resources

```cpp
// Internal helper methods called by Initialize():
CreateCommandQueues();     // Direct, Copy, Compute queues
CreateDescriptorHeaps();   // CBV/SRV/UAV, RTV, DSV, Sampler heaps
CreateFrameResources();    // Per-frame command allocators and fences
DetectCapabilities();      // Feature detection (mesh shaders, bindless, etc.)
DetectDXRSupport();        // DXR tier detection via ID3D12Device5
```

### Complete Initialization Example

```cpp
Spark::RHI::RHIDeviceDesc desc;
desc.enableDebugLayer = true;
desc.enableGPUValidation = false;  // Enable only when debugging GPU issues
desc.preferredAdapter = 0;          // 0 = auto-select best GPU
desc.forceSoftware = false;         // true = force WARP software rendering

auto device = std::make_unique<Spark::RHI::D3D12::D3D12Device>();
if (!device->Initialize(desc))
{
    LOG_ERROR("D3D12 device initialization failed");
    return false;
}

LOG_INFO("D3D12 device: {}", device->GetDeviceInfo());
LOG_INFO("DXR supported: {}", device->GetDXRDevice() != nullptr);
LOG_INFO("Software device: {}", device->IsSoftwareDevice());
```

---

## Resource Barrier Management

D3D12 requires explicit resource state transitions via barriers. The `D3D12CommandList` handles this through the RHI abstraction:

### Common Resource State Transitions

```cpp
// Transition a render target for rendering
D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    renderTarget.Get(),
    D3D12_RESOURCE_STATE_PRESENT,           // From: presentation
    D3D12_RESOURCE_STATE_RENDER_TARGET      // To: render target
);
commandList->ResourceBarrier(1, &barrier);

// After rendering, transition back for presentation
barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    renderTarget.Get(),
    D3D12_RESOURCE_STATE_RENDER_TARGET,
    D3D12_RESOURCE_STATE_PRESENT
);
commandList->ResourceBarrier(1, &barrier);
```

### Barrier Batching

Multiple barriers should be batched into a single call to minimize GPU overhead:

```cpp
D3D12_RESOURCE_BARRIER barriers[3] = {
    CD3DX12_RESOURCE_BARRIER::Transition(tex0.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET),
    CD3DX12_RESOURCE_BARRIER::Transition(tex1.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    CD3DX12_RESOURCE_BARRIER::UAV(uavResource.Get())
};
commandList->ResourceBarrier(3, barriers);
```

### Initial Resource States

The `D3D12Device` selects appropriate initial states based on buffer access patterns:

```cpp
// GetInitialResourceState() maps RHI access flags to D3D12 states:
// RHIBufferAccess::Vertex     -> D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
// RHIBufferAccess::Index      -> D3D12_RESOURCE_STATE_INDEX_BUFFER
// RHIBufferAccess::Constant   -> D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
// RHIBufferAccess::Storage    -> D3D12_RESOURCE_STATE_UNORDERED_ACCESS
// RHIBufferAccess::CopyDest   -> D3D12_RESOURCE_STATE_COPY_DEST
```

---

## Root Signature Setup

### Default Root Signature

The engine provides a default root signature via `CreateDefaultRootSignature()` that covers the majority of shader needs:

```cpp
// Root parameter layout:
// [0] CBV table:     b0-b13 (14 constant buffers, all shader stages)
// [1] SRV table:     t0-t31 (32 textures/buffers, pixel shader)
// [2] Sampler table: s0-s15 (16 samplers, pixel shader)
// [3] UAV table:     u0-u7  (8 UAVs, all shader stages)

ComPtr<ID3D12RootSignature> rootSig = device->CreateDefaultRootSignature();
```

### Custom Root Signatures

For specialized shaders (compute, raytracing), create custom root signatures from serialized blobs:

```cpp
// Compile a root signature from HLSL
ID3DBlob* serializedRootSig = nullptr;
D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
                             &serializedRootSig, nullptr);

// Create via D3D12Device helper
auto customRootSig = device->CreateRootSignature(
    serializedRootSig->GetBufferPointer(),
    serializedRootSig->GetBufferSize()
);
```

---

## Shader Model 6.x Features

The D3D12 backend detects and exposes advanced shader features through `RHIDeviceCapabilities`:

| Feature | Detection Method | Capability Flag |
|---------|-----------------|-----------------|
| DXR 1.0+ Raytracing | `ID3D12Device5` + `OPTIONS5` | `rayTracingSupport` |
| Mesh Shaders | `D3D12_OPTIONS7` | `meshShaderSupport` |
| Bindless Resources | `RESOURCE_BINDING_TIER_3` | `bindlessResourceSupport` |
| Conservative Rasterization | `D3D12_OPTIONS` | `conservativeRasterSupport` |

```cpp
const auto& caps = device->GetCapabilities();

if (caps.rayTracingSupport)
{
    // DXR is available -- use ID3D12Device5 for BLAS/TLAS creation
    ID3D12Device5* dxrDevice = device->GetDXRDevice();
    // Build acceleration structures, create ray tracing pipelines
}

if (caps.meshShaderSupport)
{
    // Use mesh/amplification shaders for GPU-driven rendering
}

if (caps.bindlessResourceSupport)
{
    // Tier 3 binding allows indexing into descriptor heaps from shaders
    // Use the 1M CBV/SRV/UAV heap directly as a bindless resource array
}
```

---

## Debugging with PIX and RenderDoc

### PIX Event Markers

The backend inserts named regions into command lists for GPU profiling:

```cpp
// PIX markers are inserted via the RHI command list interface
commandList->BeginEvent("ShadowPass");
// ... shadow rendering commands ...
commandList->EndEvent();

commandList->BeginEvent("GBuffer");
// ... geometry pass commands ...
commandList->EndEvent();
```

### DRED (Device Removed Extended Data)

When enabled, DRED provides detailed diagnostics after a device-lost crash:

```cpp
// DRED is activated automatically in debug builds
// After a device-lost event, query the DRED data:
// - Which command was executing when the device was removed
// - Auto-breadcrumbs showing the last successful commands
// - Page fault information for invalid memory access
```

### Info Queue Filtering

`m_infoQueue` exists only when `enableDebugLayer` was requested and the layer is
installed. Its storage filter denies INFO and MESSAGE severities; no message IDs
are suppressed and no severities are promoted. See
[Debug and Validation](#debug-and-validation) for the counts and break policy.

---

## Deferred Deletion Deep Dive

The deferred deletion system is critical for D3D12 correctness. Unlike D3D11, destroying a resource while the GPU is still using it causes undefined behavior.

### How It Works

Every buffer and texture that `D3D12Device` creates holds a `std::weak_ptr` to the device's
`D3D12DeferredReleaseQueue` (`D3D12Types.h`). The resource destructors are the single release
route, so a `unique_ptr` reset, `RHIAdapter::DestroyTexture` and asset unload all defer:

```cpp
// ~D3D12Texture(): hand the resource and its SRV/RTV/DSV/UAV slots to the queue.
queue->Enqueue(std::move(m_resource), descriptors);   // tagged GetCurrentValue() + 1

// BeginFrame() -> ProcessDeferredReleases() -> D3D12DeferredReleaseQueue::Process():
while (!m_entries.empty() && m_entries.front().fenceValue <= fence.GetCompletedValue())
{
    // Descriptor slots return to their heaps only now, after the GPU is done with them,
    // then the ComPtr drops the last reference.
    ReleaseEntry(m_entries.front());
    m_entries.pop_front();
}
```

The tag is the value the *next* `Signal()` will produce, so every submission made before the
destroy completes first. `Shutdown()` waits for idle, calls `ReleaseAll()` and resets the queue;
a resource destroyed after that (or a swap-chain back buffer, which has no queue) releases
immediately. `D3D12Device::GetPendingReleaseCount()` exposes the queue depth for tests.

### Frame Resource Management

Each frame has its own `FrameResources` containing a command allocator and a fence value:

```cpp
// MAX_FRAMES_IN_FLIGHT = 2 (double buffering)
// m_frameResources[0] and m_frameResources[1] alternate each frame

// MoveToNextFrame():
// 1. Signal the fence on the direct queue with the current frame's fence value
// 2. Advance m_currentFrameIndex = (m_currentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT
// 3. Wait for the next frame's previous fence value to complete
// 4. Reset that frame's command allocator (now safe -- GPU finished with it)
```

---

## Advanced Performance Tips

### GPU Memory Budget Monitoring

```cpp
// Query VRAM budget via DXGI
DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
ComPtr<IDXGIAdapter3> adapter3;
device->GetAdapter()->QueryInterface(IID_PPV_ARGS(&adapter3));
adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);

LOG_INFO("VRAM: {} MB used / {} MB budget",
         memInfo.CurrentUsage / (1024 * 1024),
         memInfo.Budget / (1024 * 1024));
```

### Minimizing State Changes

```
DO:
  - Sort draw calls by pipeline state to minimize root signature and PSO swaps
  - Use the default root signature for all standard materials
  - Batch descriptor copies using CopyDescriptorsSimple

DON'T:
  - Switch root signatures between every draw call
  - Create new pipeline states at runtime (cache them)
  - Allocate descriptors one at a time (allocate ranges)
```

### Copy Queue Best Practices

```cpp
// Use the copy queue for texture uploads to overlap with rendering:
ID3D12CommandQueue* copyQueue = device->GetCopyQueue();

// 1. Record upload commands on a copy command list
// 2. Submit to copy queue (runs concurrently with direct queue)
// 3. Insert a fence on the copy queue
// 4. Wait for that fence on the direct queue before using the texture

// This overlaps GPU rendering with texture data transfer
```

### Compute Queue Usage

```cpp
// Async compute runs independently of the graphics pipeline:
ID3D12CommandQueue* computeQueue = device->GetComputeQueue();

// Good candidates for async compute:
// - Post-processing passes (bloom, SSAO reduction)
// - Particle simulation
// - Culling and indirect draw argument generation
// - Virtual texture feedback analysis
```

---

## RHI Statistics

The device tracks per-frame rendering statistics:

```cpp
const RHIStatistics& stats = device->GetStatistics();

LOG_INFO("Draw calls: {}", stats.drawCalls);
LOG_INFO("Dispatch calls: {}", stats.dispatchCalls);
LOG_INFO("Triangles: {}", stats.trianglesRendered);
LOG_INFO("Buffer creates: {}", stats.buffersCreated);
LOG_INFO("Texture creates: {}", stats.texturesCreated);

// Reset at the start of each frame
device->ResetStatistics();
```

---

## MinGW Cross-Compilation

MinGW headers are too old for the complete D3D12 backend, including `ID3D12Device5`, debug-layer types, mesh shaders, and variable-rate shading. The root CMake build excludes D3D12 and DXR sources and defines `SPARK_NO_D3D12` for MinGW. D3D11 may be exercised under Wine + DXVK as a separate development route; it does not validate D3D12 or certify this backend.

---

## See Also

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md) — Backend-agnostic graphics interface
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Render pipelines and materials
- [DXR Raytracing](DXR-Raytracing.md) — Ray tracing built on D3D12
- [Upscaling (DLSS/FSR)](Upscaling-System.md) — Upscaling techniques
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — Shader compilation for D3D12
- [Profiler and Debugging](../advanced/Profiler-and-Debugging.md) — GPU profiling and debug tools
