# OpenGL Backend

> **Release boundary:** This page documents an experimental implementation outside
> the blocked and uncertified `stable-v1` product scope (Windows 11 x64/MSVC
> v143 with D3D11 or Windows NullRHI and C++ modules). It is not a supported or
> release-certified backend.

Experimental OpenGL RHI development implementation. The direct EGL/GLX device
path requests a 4.5 context, while the SDL runtime/editor hosts request 3.3;
source-level DSA and SPIR-V paths still depend on the context and driver that is
actually created. It is not a generic or certified fallback.

## Overview

- **Namespace:** `Spark::RHI::OpenGL`
- **Files:** `Graphics/RHI/OpenGL/OpenGLDevice.{h,cpp}` (~450 LOC header).
- **Guard:** `#ifdef SPARK_OPENGL_SUPPORT` (CMake toggle `ENABLE_OPENGL=ON`).
- **Linkage:** GLAD loader (bundled in `ThirdParty/`) + SDL2 for context creation.
- **Context/feature boundary:** EGL/GLX requests OpenGL 4.5; SDL hosts request
  3.3. DSA and SPIR-V binary-shader calls exist in source but are not a blanket
  OpenGL 4.6 runtime guarantee.

## Architecture

| Class | Purpose | RHI interface |
|-------|---------|---------------|
| `GLDevice` | Context owner, resource factory | `IRHIDevice` |
| `GLBuffer` | `glCreateBuffers` + DSA upload | `IRHIBuffer` |
| `GLTexture` | `glCreateTextures` + optional framebuffer attachment | `IRHITexture` |
| `GLShader` | Program or separable shader object | `IRHIShader` |
| `GLSampler` | `glCreateSamplers` state | `IRHISampler` |
| `GLPipelineState` | VAO + pipeline composition | `IRHIPipelineState` |
| `GLSwapChain` | SDL2-backed double-buffer swap | `IRHISwapChain` |
| `GLCommandList` | Immediate-mode translation (no real GL command buffer) | `IRHICommandList` |

OpenGL is single-threaded — command recording is serialized on the render thread.

## Features

- **Direct state access (DSA)** — all resource creation uses the DSA variants (`glCreateTextures`, `glNamedBufferStorage`, etc.). Cleaner than bind-to-modify.
- **SPIR-V ingest** — shader loading accepts compiled SPIR-V binaries via `glShaderBinary(GL_SHADER_BINARY_FORMAT_SPIR_V)`, so the offline shader cook path produces one binary format for both OpenGL and Vulkan.
- **Mesa `llvmpipe` development route** — requires explicit Mesa/driver and headless-display configuration; it is not a generic CI or release-certified fallback. See the [RHI fallback chain](RHI-Abstraction-Layer.md#nullrhidevice-selection-and-bridge-failover).
- **KHR_debug callback** — validation messages routed through `Spark::Logger` in debug builds.
- **Persistent-mapped buffers** — `GL_MAP_PERSISTENT_BIT` used for per-frame transient UI / debug-draw streams.
- **Framebuffer object (FBO) abstraction** — `GLTexture::SetFramebuffer` caches the FBO that owns this texture as a color/depth attachment, avoiding per-draw-call FBO churn.

## Capability detection

```cpp
auto& caps = device->GetCapabilities();
caps.maxTextureSize           // GL_MAX_TEXTURE_SIZE
caps.computeShaderSupport     // true on 4.3+
caps.bindlessResourceSupport  // GL_ARB_bindless_texture (NVIDIA)
caps.meshShaderSupport        // GL_NV_mesh_shader (experimental)
```

## Factory registration

```cpp
// RHIFactory.cpp
case GraphicsBackend::OpenGL:
    device = std::make_unique<OpenGL::GLDevice>();
    break;
```

`RHIFactory::GetRecommendedBackend()` selects a preferred compiled and available backend; it does not retry device startup. `RHIBridge::Initialize` tries available GPU candidates after device or swap-chain failure and creates `NullRHIDevice` only if they all fail. OpenGL is one optional candidate, not a `NullRHI` fallback tier.

## Limitations

- **No timeline semaphores** — CPU/GPU sync via `glFenceSync` / `glClientWaitSync`.
- **No bindless (standard)** — vendor-specific extensions only.
- **No ray tracing, no mesh shaders (portable)** — upgrade to Vulkan or D3D12 for those.
- **Single thread** — render commands must run on the thread that created the context.

## Related

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md)
- [Vulkan Backend](Vulkan-Backend.md) — experimental Linux development alternative when compiled.
- [D3D11 Backend](D3D11-Backend.md) — Windows equivalent tier.
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — how shaders are built into the SPIR-V blobs that OpenGL consumes.
