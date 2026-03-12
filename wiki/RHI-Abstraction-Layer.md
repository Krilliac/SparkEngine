# RHI Abstraction Layer

## Overview

The **Rendering Hardware Interface (RHI)** is SparkEngine's backend-agnostic graphics abstraction layer. It decouples all rendering code from any specific graphics API, allowing the engine to target DirectX 11, DirectX 12, Vulkan, OpenGL, and Metal through a single set of abstract interfaces. Application-level rendering code interacts exclusively with the RHI; the concrete backend is selected at runtime via the `RHIFactory`.

The RHI lives under `SparkEngine/Source/Graphics/RHI/` and is consumed by the higher-level `GraphicsEngine`, `RenderPipeline`, and `RenderGraph` systems. A single master include (`RHI.h`) pulls in the entire abstraction:

```cpp
#include "Graphics/RHI/RHI.h"
```

### Design goals

- **Backend transparency** -- rendering code never references D3D11, Vulkan, or OpenGL types directly.
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
        +---> D3D11Device       (Windows, production)
        +---> D3D12Device       (Windows 10+, production)
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
| `DetectAvailableBackends()` | Returns a `std::vector<GraphicsBackend>` of all backends compiled in and available at runtime. |
| `GetRecommendedBackend()` | Returns the platform's preferred backend (D3D11 on Windows, Vulkan on Linux). |
| `IsBackendAvailable(backend)` | Checks whether a specific backend can be created. |
| `GetBackendName(backend)` | Returns a human-readable string (e.g. `"DirectX 11"`, `"Vulkan"`). |

### Compile-time backend availability

Backend inclusion is controlled by preprocessor defines and platform detection:

- **D3D11 / D3D12** -- available when `_WIN32` is defined (always on Windows builds).
- **Vulkan** -- available when `SPARK_VULKAN_SUPPORT` is defined at CMake configure time.
- **OpenGL** -- available when `SPARK_OPENGL_SUPPORT` is defined at CMake configure time.
- **Metal** -- defined in the `GraphicsBackend` enum but requires the Metal backend subdirectory (macOS experimental).

---

## Backend Selection Logic

When `GraphicsBackend::Auto` is passed to `CreateDevice()`, the factory calls `GetRecommendedBackend()` which applies the following priority:

1. **Windows**: D3D11 (stable, fully featured).
2. **Linux**: Vulkan (if `SPARK_VULKAN_SUPPORT` is enabled).
3. **Fallback**: the first backend returned by `DetectAvailableBackends()`.
4. **No backend**: returns `GraphicsBackend::None` and the engine runs without rendering.

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
| `ReflectSPIRV()` | Extract binding and input attribute information from SPIR-V bytecode. |
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
| **DirectX 11** | Windows | Production | `_WIN32` (automatic) | Primary backend. Fully featured. Always available on Windows builds. |
| **DirectX 12** | Windows 10+ | Production | `_WIN32` (automatic) | Supports advanced features (mesh shaders, ray tracing, VRS). See [D3D12 Backend](D3D12-Backend). |
| **Vulkan** | Windows, Linux, macOS (MoltenVK) | Experimental | `SPARK_VULKAN_SUPPORT` | Cross-platform. Requires Vulkan SDK at build time. |
| **OpenGL** | Windows, Linux | Experimental | `SPARK_OPENGL_SUPPORT` | Fallback for older hardware. Basic keyword-level HLSL-to-GLSL translation. |
| **Metal** | macOS | Experimental | N/A (planned) | Enum value exists; backend subdirectory is stubbed. |

---

## Thread Safety

- **IRHIDevice** -- main thread only for resource creation and destruction. `std::atomic` frame state for cross-thread queries.
- **IRHICommandList** -- main thread only (matches `GraphicsEngine` contract).
- **RHIAdapter** -- main thread only. Mirrors the legacy `GraphicsEngine` threading model.
- **RHIFactory** free functions (`DetectAvailableBackends`, `IsBackendAvailable`, etc.) -- safe to call from any thread (read-only queries).

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) -- high-level rendering architecture, `GraphicsEngine`, post-processing
- [D3D12 Backend](D3D12-Backend) -- DirectX 12 backend implementation details
- [DXR Raytracing](DXR-Raytracing) -- hardware ray tracing via DXR
- [Shader Pipeline](Shader-Pipeline) -- shader authoring, compilation, and hot-reload
- [Architecture Overview](Architecture-Overview) -- engine-wide architecture and subsystem map
