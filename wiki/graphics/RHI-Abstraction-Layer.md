# RHI Abstraction Layer

> **Release boundary:** D3D11 and the no-render NullRHI path are declared in
> `stable-v1` only on Windows 11 x64, and that profile remains blocked and
> uncertified. D3D12, Vulkan, OpenGL, Metal, and NullRHI on other hosts are
> experimental or uncertified implementation paths outside the profile.

## Overview

The **Rendering Hardware Interface (RHI)** provides backend-neutral interfaces used by backend-aware rendering paths. The primary Windows renderer still contains substantial native D3D11 integration, so the RHI does not yet decouple all rendering code and cross-backend parity is incomplete. `RHIFactory` selects among the implementations compiled into a build.

The RHI lives under `SparkEngine/Source/Graphics/RHI/` and is consumed by the higher-level `GraphicsEngine`, `RenderPipeline`, and `RenderGraph` systems. A single master include (`RHI.h`) pulls in the entire abstraction:

```cpp
#include "Graphics/RHI/RHI.h"
```

### Design goals

- **Backend-transparency goal** -- RHI-aware paths avoid native backend types; the current Windows renderer still has direct D3D11 integration.
- **Runtime backend selection** -- the `GraphicsBackend` enum and `RHIFactory` allow switching APIs without recompilation.
- **Minimal overhead** -- thin virtual interface; backend implementations map directly to native API calls.
- **Resource safety** -- RAII lifetime management; the `RHIAdapter` tracks all created resources and destroys them on shutdown.
- **Capability querying** -- `RHIDeviceCapabilities` reports hardware features (ray tracing, mesh shaders, bindless resources) so higher-level systems can adapt.

---

## Architecture

The RHI is organised into several layers, each with a distinct responsibility:

```
  Application / Game Code
        |
        v
  RHIBridge / RHIAdapter       <-- High-level convenience wrappers
        |
        v
  IRHIDevice / IRHICommandList  <-- Abstract device & command recording
        |
        v
  RHIFactory                    <-- Backend detection & device creation
        |
        +---> D3D11Device       (Windows implementation; profile target only on Windows 11 x64, blocked/uncertified)
        +---> D3D12Device       (Windows 10+; experimental/outside stable-v1)
        +---> VulkanDevice      (Windows/Linux/macOS via MoltenVK, experimental)
        +---> GLDevice          (Windows/Linux, experimental)
        +---> MetalDevice       (macOS, experimental)
```

### Key source files

| File | Purpose |
|------|---------|
| `RHI.h` | Master include -- pulls in all RHI headers |
| `RHITypes.h` | Enumerations, descriptor structs, capability/statistics types |
| `RHIResources.h` | Abstract resource interfaces (`IRHIBuffer`, `IRHITexture`, `IRHIShader`, `IRHISampler`, `IRHIPipelineState`) |
| `RHIDevice.h` | `IRHIDevice` (device), `IRHICommandList` (command recording), `IRHISwapChain` (presentation) |
| `RHIFactory.h` / `.cpp` | `CreateDevice()`, `DetectAvailableBackends()`, shader compilation utilities |
| `RHIBridge.h` / `.cpp` | High-level integration bridge with swap chain, depth buffer, and shader cache management |
| `RHIAdapter.h` / `.cpp` | Legacy `GraphicsEngine` adapter -- translates old API calls into RHI operations with resource tracking |

---

## RHI Factory

`RHIFactory` is the entry point for creating a device. It handles platform detection, backend availability checks, and device instantiation.

### Creating a device

```cpp
// Auto-detect the best backend for the current platform
auto device = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::Auto);

// Or request a specific backend
auto device = Spark::RHI::CreateDevice(Spark::RHI::GraphicsBackend::Vulkan);
```

### Key factory functions

| Function | Description |
|----------|-------------|
| `CreateDevice(backend)` | Create an `IRHIDevice` for the given backend. `Auto` picks the recommended one. Returns `nullptr` if unavailable. |
| `DetectAvailableBackends()` | Enumerates backends included by the current platform and compile-time defines; it does not probe the runtime or initialize a device. |
| `GetRecommendedBackend()` | Applies the environment/gVisor rules and compile-time list, then prefers D3D11 on Windows, Metal on Apple hosts, or Vulkan elsewhere. |
| `IsBackendAvailable(backend)` | Checks membership in the compile-time backend list; it does not prove that device initialization will succeed. |
| `GetBackendName(backend)` | Returns a human-readable string (e.g. `"DirectX 11"`, `"Vulkan"`). |

### Compile-time backend availability

Backend inclusion is controlled by preprocessor defines and platform detection:

- **D3D11** -- included when `_WIN32` is defined.
- **D3D12** -- included when `_WIN32` is defined and `SPARK_NO_D3D12` is not defined (the MinGW path defines it).
- **Vulkan** -- available when `SPARK_VULKAN_SUPPORT` is defined at CMake configure time.
- **OpenGL** -- available when `SPARK_OPENGL_SUPPORT` is defined at CMake configure time.
- **Metal** -- Objective-C++ implementation compiled under `SPARK_METAL_SUPPORT` on macOS; experimental and outside `stable-v1`.
- **None (NullRHIDevice)** -- always-compiled no-render device implementation. The factory selects it for an explicit `None`, the recognized null/headless environment override, gVisor, or an empty compile-time backend list. It is in `stable-v1` only on Windows 11 x64; other hosts are uncertified.

---

## Backend Selection Logic

When `GraphicsBackend::Auto` is passed to `CreateDevice()`, the factory calls `GetRecommendedBackend()` in this order:

1. Honor a recognized `SPARK_RHI_BACKEND` override when that backend is in the compile-time list; `null`, `none`, and `headless` select `None` directly.
2. Select `None` under gVisor.
3. Select `None` when the compile-time backend list is empty.
4. Prefer D3D11 on Windows, Metal on Apple hosts, or Vulkan on other hosts when the preferred backend is compiled in.
5. Otherwise select the first compile-time backend in the list.

The bare `RHIFactory::CreateDevice()` call performs selection and construction
only; it does not initialize the device or retry another backend. The higher-level
`RHIBridge::Initialize()` adds runtime failover: it orders the compiled/available
GPU candidates with the requested backend first, tries device initialization and
swap-chain creation for each, and continues after either failure. If every GPU
candidate fails, the bridge creates `NullRHIDevice` and enters headless mode.
That resilience mechanism is not release certification for the fallback backend
or host.

### NullRHIDevice Selection and Bridge Failover

When `GraphicsBackend::None` is selected, `RHIFactory::CreateDevice()` returns a
`NullRHIDevice` instead of `nullptr`. `RHIBridge::Initialize()` also reaches the
same device when its window handle forces the no-render path or every GPU
candidate fails. The bridge then enters **headless mode**, skipping swap-chain
and depth-buffer creation:

```cpp
// RHIBridge automatically enters headless mode when NullRHIDevice is active
RHIBridge bridge;
bridge.Initialize(nullptr, 800, 600, GraphicsBackend::Auto);
if (bridge.IsHeadless()) {
    // No swap chain, no depth buffer, but device is valid
    // No rendering occurs. Each non-render subsystem still needs its own host wiring and evidence.
}
```

### Software Rendering via OpenGL + llvmpipe

On Linux, the OpenGL backend can render entirely on CPU using Mesa's **llvmpipe** software rasterizer (LLVM JIT-compiled). This provides real GL 4.5 rendering without a GPU:

```bash
# Start a virtual display (if no physical display)
Xvfb :99 -screen 0 1024x768x24 &

# Run with CPU-based software rendering
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./SparkEngine
```

The OpenGL backend contains a Linux GLX PBuffer and FBO-backed off-screen path. llvmpipe is an explicitly configured development route; it does not establish identical behavior or release parity with a GPU-backed path.

---

## IRHIDevice Interface

`IRHIDevice` is the central abstraction. Every backend implements this interface. It covers the full lifecycle of GPU interaction: initialisation, resource management, command submission, and capability reporting.

### Lifecycle

| Method | Description |
|--------|-------------|
| `Initialize(desc)` | Create the native device with debug layers, validation, and application metadata from `RHIDeviceDesc`. |
| `Shutdown()` | Tear down the device and release all native resources. |
| `BeginFrame()` / `EndFrame()` | Frame delimiters. Must bracket all per-frame rendering work. |
| `WaitForIdle()` | Block until the GPU has finished all submitted work (used during shutdown or resize). |

### Resource creation and destruction

```cpp
IRHIBuffer*        CreateBuffer(const RHIBufferDesc& desc);
IRHITexture*       CreateTexture(const RHITextureDesc& desc);
IRHIShader*        CreateShader(const RHIShaderDesc& desc);
IRHISampler*       CreateSampler(const RHISamplerDesc& desc);
IRHIPipelineState* CreatePipelineState(const RHIPipelineStateDesc& desc,
                                       IRHIShader* vs, IRHIShader* ps);
```

Each `Create` method has a corresponding `Destroy` method. The `RHIAdapter` wrapper tracks created resources and destroys them automatically on `Shutdown()`.

### Resource updates

| Method | Description |
|--------|-------------|
| `MapBuffer(buffer)` | Map a buffer for CPU write access. Returns `void*`. |
| `UnmapBuffer(buffer)` | Unmap a previously mapped buffer. |
| `UpdateBuffer(buffer, data, size, offset)` | Copy CPU data into a buffer at the given offset. |
| `UpdateTexture(texture, data, mipLevel, arraySlice)` | Upload pixel data to a texture mip/slice. |

### Swap chain

`IRHIDevice::CreateSwapChain(desc)` creates an `IRHISwapChain` from an `RHISwapChainDesc`:

```cpp
RHISwapChainDesc swapDesc;
swapDesc.windowHandle = hwnd;
swapDesc.width        = 1920;
swapDesc.height       = 1080;
swapDesc.format       = PixelFormat::R8G8B8A8_UNORM;
swapDesc.bufferCount  = 2;
swapDesc.vsync        = true;

auto swapChain = device->CreateSwapChain(swapDesc);
```

The `IRHISwapChain` interface exposes `Present(vsync)`, `Resize(w, h)`, `GetBackBuffer()`, and buffer index queries.

### Device info and capabilities

`GetCapabilities()` returns an `RHIDeviceCapabilities` struct. `GetStatistics()` returns per-frame `RHIStatistics`. `GetDeviceInfo()` returns a human-readable device description string.

---

## Resource Types

All GPU resources inherit from `IRHIResource`, which provides debug naming (`GetDebugName` / `SetDebugName`) and validity checking (`IsValid`).

### IRHIBuffer

Represents vertex buffers, index buffers, constant buffers, structured buffers, and indirect argument buffers. Configured via `RHIBufferDesc`:

- **`size`** -- total size in bytes.
- **`stride`** -- per-element stride (used for vertex and structured buffers).
- **`usage`** -- bitmask of `RHIBufferUsage` flags (`Vertex`, `Index`, `Constant`, `Structured`, `IndirectArgs`, `Storage`, `CopySrc`, `CopyDst`).
- **`access`** -- `Static` (GPU-only), `Dynamic` (CPU-write per frame), `Staging` (CPU read/write), `ReadBack` (GPU-write, CPU-read).

### IRHITexture

Represents 1D, 2D, 3D, cube, and array textures. Configured via `RHITextureDesc`:

- **`type`** -- `Texture1D`, `Texture2D`, `Texture3D`, `TextureCube`, `Texture2DArray`, `TextureCubeArray`.
- **`usage`** -- bitmask of `RHITextureUsage` flags (`ShaderResource`, `RenderTarget`, `DepthStencil`, `UnorderedAccess`, `TransferSrc`, `TransferDst`).
- **`format`** -- any value from the `PixelFormat` enum (35+ formats including UNORM, FLOAT, SRGB, BC compressed, and depth/stencil variants).

Native views are accessible through `GetShaderResourceView()`, `GetRenderTargetView()`, and `GetDepthStencilView()`.

### IRHIShader

Represents a compiled shader stage. Configured via `RHIShaderDesc` which accepts pre-compiled bytecode or source code, an entry point, shader language (`HLSL`, `GLSL`, `SPIRV`, `Auto`), and preprocessor defines.

### IRHISampler

Represents a texture sampler state. Configured via `RHISamplerDesc` with filter modes (`Nearest`, `Linear`, `Anisotropic`), address modes (`Wrap`, `Clamp`, `Mirror`, `Border`, `MirrorOnce`), comparison function, LOD range, and anisotropy level.

### IRHIPipelineState

Represents a complete graphics pipeline state combining:

- **Input layout** (`RHIInputLayoutDesc`) -- vertex attribute descriptions.
- **Rasteriser state** (`RHIRasterizerDesc`) -- fill mode, cull mode, depth bias, scissor/multisample enables.
- **Blend state** (`RHIBlendDesc`) -- per-render-target blend settings, alpha-to-coverage.
- **Depth-stencil state** (`RHIDepthStencilDesc`) -- depth test/write, stencil operations.
- **Topology** -- `TriangleList`, `LineList`, `PatchList`, etc.
- **Render target formats** and **sample count**.

---

## Command Lists

`IRHICommandList` records GPU commands for later execution. The device provides two kinds:

- **Immediate command list** (`GetImmediateCommandList()`) -- executes commands immediately on the GPU timeline.
- **Deferred command list** (`CreateDeferredCommandList()`) -- records commands for later submission via `ExecuteCommandList()`.

### Command categories

| Category | Methods |
|----------|---------|
| **Lifecycle** | `Begin()`, `End()`, `Reset()` |
| **Render targets** | `SetRenderTargets()`, `ClearRenderTarget()`, `ClearDepthStencil()` |
| **Viewport/scissor** | `SetViewport()`, `SetScissorRect()` |
| **Pipeline** | `SetPipelineState()`, `SetPrimitiveTopology()` |
| **Resource binding** | `SetVertexBuffer()`, `SetIndexBuffer()`, `SetConstantBuffer()`, `SetShaderResource()`, `SetSampler()` |
| **Draw** | `Draw()`, `DrawIndexed()`, `DrawInstanced()`, `DrawIndexedInstanced()` |
| **Compute** | `Dispatch(x, y, z)` |
| **Debug** | `BeginEvent()`, `EndEvent()`, `SetMarker()` |

Resource binding methods accept an `RHIShaderStage` parameter (`Vertex`, `Pixel`, `Geometry`, `Hull`, `Domain`, `Compute`) and a slot index, matching the HLSL register model.

---

## Swap Chain Management

The `IRHISwapChain` abstraction handles presentation and back buffer access:

```cpp
swapChain->Present(vsync);           // Flip / present
swapChain->Resize(newWidth, newHeight); // Handle window resize
IRHITexture* backBuffer = swapChain->GetBackBuffer();
uint32_t bufferIndex = swapChain->GetCurrentBufferIndex();
```

The `RHIBridge` wraps swap chain creation and management with automatic depth buffer recreation on resize.

---

## Capability Detection

The `RHIDeviceCapabilities` struct, returned by `IRHIDevice::GetCapabilities()`, reports hardware and API feature support:

| Field | Type | Description |
|-------|------|-------------|
| `deviceName` | `string` | GPU name (e.g. "NVIDIA GeForce RTX 4090") |
| `vendorName` | `string` | GPU vendor |
| `apiVersion` | `string` | Backend API version string |
| `dedicatedVideoMemory` | `uint64_t` | Dedicated VRAM in bytes |
| `sharedSystemMemory` | `uint64_t` | Shared system memory in bytes |
| `tessellationSupport` | `bool` | Hull/domain shader support |
| `computeShaderSupport` | `bool` | Compute shader support |
| `geometryShaderSupport` | `bool` | Geometry shader support |
| `meshShaderSupport` | `bool` | Mesh shader support (D3D12/Vulkan) |
| `rayTracingSupport` | `bool` | Hardware ray tracing (DXR / Vulkan RT) |
| `variableRateShadingSupport` | `bool` | VRS tier support |
| `bindlessResourceSupport` | `bool` | Bindless/descriptor indexing |
| `conservativeRasterSupport` | `bool` | Conservative rasterisation |
| `multiDrawIndirectSupport` | `bool` | Multi-draw indirect |
| `maxTextureSize` | `uint32_t` | Maximum texture dimension (default 16384) |
| `maxRenderTargets` | `uint32_t` | Maximum simultaneous render targets (default 8) |
| `maxMSAASamples` | `uint32_t` | Maximum MSAA sample count |
| `maxAnisotropy` | `float` | Maximum anisotropic filtering level |

Higher-level systems (e.g. the render pipeline, post-processing stack) query these capabilities to enable or disable features at runtime.

### Per-frame statistics

`RHIStatistics` tracks per-frame GPU workload metrics: draw calls, dispatch calls, triangles rendered, vertices processed, texture/buffer binds, pipeline state changes, render target changes, GPU memory usage, and GPU frame time.

---

## Shader Compilation and Cross-Compilation

`RHIFactory` includes a cross-platform shader compilation pipeline:

| Function | Description |
|----------|-------------|
| `CompileShader(options)` | Compile from source or file, auto-detecting language and target. Returns `ShaderCompileResult` with bytecode or error messages. |
| `CrossCompileHLSLtoGLSL()` | Basic HLSL-to-GLSL keyword translation (for simple shaders; complex shaders need SPIRV-Cross). |
| `CrossCompileHLSLtoSPIRV()` | HLSL-to-SPIR-V via DXC (requires `dxcompiler.dll` integration). |
| `CompileGLSLtoSPIRV()` | GLSL-to-SPIR-V via glslang (requires glslang library integration). |
| `ReflectSPIRV()` | Placeholder validation: checks the SPIR-V header and currently returns empty reflection because SPIRV-Reflect is not integrated. |
| `LoadPrecompiledShader()` / `SaveCompiledShader()` | Load/save pre-compiled shader bytecode to disk. |

The `ShaderCache` class (in `RHIBridge.h`) manages shader variants per backend, loading the correct HLSL, GLSL, or SPIR-V file depending on the active `GraphicsBackend`.

---

## RHI Bridge and Adapter

Two high-level wrappers simplify RHI usage:

### RHIBridge

`RHIBridge` provides a turnkey integration path. It owns the device, swap chain, and depth buffer, and exposes convenience methods for common resource creation patterns:

```cpp
Spark::RHI::RHIBridge bridge;
bridge.Initialize(hwnd, 1920, 1080, GraphicsBackend::Auto);

bridge.BeginFrame();
auto* cmd = bridge.GetCommandList();
// ... issue draw commands ...
bridge.EndFrame();
bridge.Present(true);
```

Convenience resource creation includes `CreateVertexBuffer`, `CreateIndexBuffer`, `CreateConstantBuffer`, `CreateTexture2D`, `CreateDepthBuffer`, `CreateRenderTarget`, and preset sampler creation (`CreateSamplerLinearWrap`, `CreateSamplerAnisotropic`, etc.).

### RHIAdapter

`RHIAdapter` is the legacy compatibility layer. It takes a non-owning `IRHIDevice*` and translates the old `GraphicsEngine` API (direct D3D11 calls) into abstract RHI operations. All resources created through the adapter are tracked internally and bulk-destroyed on `Shutdown()`.

The adapter is main-thread only, matching the `GraphicsEngine` thread safety contract.

---

## Render Graph Integration

The RHI integrates with SparkEngine's declarative **Render Graph** system (`SparkEngine/Source/Graphics/RenderGraph.h` and `RenderGraph/RenderGraphBuilder.h`). The render graph expresses the frame as a DAG of render passes with explicit resource dependencies.

### StandardPipelineBuilder

`StandardPipelineBuilder` constructs the engine's canonical deferred rendering pipeline:

```
Shadow --> GBuffer --> Lighting --> PostProcess --> UI --> Debug (optional)
```

Each pass declares transient resources through the `RenderGraphBuilder` DSL. The graph is compiled (topological sort, dead-code elimination, resource aliasing) and then executed each frame.

```cpp
StandardPipelineBuilder pipelineBuilder;
pipelineBuilder.Configure(config);
pipelineBuilder.SetFrameData(frameData);
pipelineBuilder.SetCallbacks(callbacks);

RenderGraph graph("MainFrame", device);
pipelineBuilder.Build(graph);
graph.Compile();
graph.Execute();
```

The `RenderGraphBuilder` integrates with the RHI through `RHIAdapter`, so all GPU work issued by the render graph flows through the abstract device interface.

---

## Backend Status Matrix

| Backend | Platform | Status | Compile Define | Notes |
|---------|----------|--------|----------------|-------|
| **DirectX 11** | Windows 11 x64 | `stable-v1` target — blocked/uncertified | `_WIN32` (automatic) | Primary implementation path; other Windows rows are development-only and certification gates remain open. |
| **DirectX 12** | Windows development builds | Experimental / outside `stable-v1` | `_WIN32` and not `SPARK_NO_D3D12` | Advanced implementation path (mesh shaders, ray tracing, VRS). See [D3D12 Backend](D3D12-Backend.md). |
| **Vulkan** | Windows, Linux, macOS (MoltenVK) | Experimental | `SPARK_VULKAN_SUPPORT` | Cross-platform. Requires Vulkan SDK at build time. |
| **OpenGL** | Windows, Linux, macOS development builds | Experimental | `SPARK_OPENGL_SUPPORT` | GL implementation path; CPU software rendering via Mesa llvmpipe requires explicit host configuration. |
| **Metal** | macOS | Experimental / outside `stable-v1` | `SPARK_METAL_SUPPORT` | Objective-C++ device, ray-tracing, interop, and readback implementations exist; no release certification. |
| **None (Null)** | Host-dependent | No-render implementation; in-profile only on Windows 11 x64 | Always available | `NullRHIDevice` rasterizes nothing. Other-host use is uncertified. |

---

## Vulkan ↔ D3D11 Parity Milestones (as of April 9, 2026)

The Vulkan backend now exposes an explicit parity milestone snapshot in `VulkanDevice::GetD3D11ParityMilestones()` and a deterministic canonical golden-scene route through `VulkanDevice::RenderCanonicalGoldenScene()`. These are validated in CI-oriented tests (`VulkanParity_*` in `Tests/TestVulkanLavapipe.cpp`).

### Milestone checklist

- ✅ Frame lifecycle (`BeginFrame`/`EndFrame` fencing + submission path)
- ✅ Resource barriers / synchronization baseline (image transitions + upload fence path)
- ✅ Descriptor binding model parity baseline (D3D11-style fixed slots mapped to Vulkan descriptor set layout; push-descriptor fast path when available)
- ✅ Shadow/deferred pass route declared through `StandardPipelineBuilder`
- ✅ Post-process route declared through `StandardPipelineBuilder`
- ✅ Canonical golden-scene render route (deterministic RGBA output for regression comparison)
- ✅ CI assertion hooks:
  - Vulkan preset assertion (`linux-gcc-release` configure cache must enable Vulkan)
  - Shader compile path assertion (`VulkanParity_ShaderCompilePath_Asserted`)

### Explicitly unsupported / not-yet-parity-complete features

Until full Vulkan parity is completed, the following remain explicitly unsupported or incomplete versus the primary D3D11 implementation:

1. Full GPU-backed golden-scene readback parity (current canonical route is deterministic and backend-owned, but not yet a full swapchain readback of the renderer in CI).
2. End-to-end Vulkan pass execution validation for every shadow/deferred/post variant (current milestone verifies route wiring and deterministic reference output, not full feature-by-feature visual equivalence).
3. Vulkan shader toolchain hard dependency in CI (DXC/glslang integration may still be optional; current gate asserts the path executes and reports deterministic outcome).

These items remain documented here by design and should be removed only when the Vulkan path is verified feature-complete against D3D11.

---

## Thread Safety

- **IRHIDevice** -- main thread only for resource creation and destruction. `std::atomic` frame state for cross-thread queries.
- **IRHICommandList** -- main thread only (matches `GraphicsEngine` contract).
- **RHIAdapter** -- main thread only. Mirrors the legacy `GraphicsEngine` threading model.
- **RHIFactory** free functions (`DetectAvailableBackends`, `IsBackendAvailable`, etc.) -- safe to call from any thread (read-only queries).

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- high-level rendering architecture, `GraphicsEngine`, post-processing
- [D3D12 Backend](D3D12-Backend.md) -- DirectX 12 backend implementation details
- [DXR Raytracing](DXR-Raytracing.md) -- hardware ray tracing via DXR
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) -- shader authoring, compilation, and hot-reload
- [Architecture Overview](../getting-started/Architecture-Overview.md) -- engine-wide architecture and subsystem map
