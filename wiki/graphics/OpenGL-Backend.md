# OpenGL Backend

Cross-platform fallback backend targeting **OpenGL 4.6**. Useful when the host has no Vulkan or D3D support — most notably Linux systems with only software rasterization (`llvmpipe`) or legacy adapters.

## Overview

- **Namespace:** `Spark::RHI::OpenGL`
- **Files:** `Graphics/RHI/OpenGL/OpenGLDevice.{h,cpp}` (~450 LOC header).
- **Guard:** `#ifdef SPARK_OPENGL_SUPPORT` (CMake toggle `ENABLE_OPENGL=ON`).
- **Linkage:** GLAD loader (bundled in `ThirdParty/`) + SDL2 for context creation.
- **Core profile:** OpenGL 4.6 core, direct-state access (DSA), SPIR-V binary shader ingest.

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
- **Mesa `llvmpipe` software rasterization** — valid headless-render fallback on CI. See the [RHI fallback chain](RHI-Abstraction-Layer.md#automatic-fallback-to-nullrhidevice).
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

Used by `RHIFactory::GetRecommendedBackend()` on Linux when `SPARK_VULKAN_SUPPORT` is disabled, and as the NullRHIDevice fallback tier immediately before the null sink.

## Limitations

- **No timeline semaphores** — CPU/GPU sync via `glFenceSync` / `glClientWaitSync`.
- **No bindless (standard)** — vendor-specific extensions only.
- **No ray tracing, no mesh shaders (portable)** — upgrade to Vulkan or D3D12 for those.
- **Single thread** — render commands must run on the thread that created the context.

## Related

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md)
- [Vulkan Backend](Vulkan-Backend.md) — preferred on Linux when available.
- [D3D11 Backend](D3D11-Backend.md) — Windows equivalent tier.
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — how shaders are built into the SPIR-V blobs that OpenGL consumes.
