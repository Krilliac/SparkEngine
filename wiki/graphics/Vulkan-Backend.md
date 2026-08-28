# Vulkan Backend

> **Release boundary:** This page documents an experimental implementation outside
> the blocked and uncertified `stable-v1` product scope (Windows 11 x64/MSVC
> v143 with D3D11 or Windows NullRHI and C++ modules). It is not a supported or
> release-certified backend.

SparkEngine has a Vulkan RHI implementation for Windows, Linux, and MoltenVK-based macOS development work. It is experimental and outside `stable-v1`, not a primary cross-platform supported renderer.

## Overview

- **Namespace:** `Spark::RHI::Vulkan`
- **Files:** `Graphics/RHI/Vulkan/VulkanDevice.{h,cpp}` (~550 LOC header), `VulkanCommandList.cpp`, `VulkanDescriptorCache.{h,cpp}`, `VulkanFormatHelpers.cpp`.
- **Guard:** `#ifdef SPARK_VULKAN_SUPPORT` (set by CMake when `ENABLE_VULKAN=ON`).
- **Linkage:** `libvulkan.so.1` / `vulkan-1.dll` via `volk` loader.
- **API level:** Vulkan 1.4 — dynamic rendering, push descriptors, timeline semaphores, synchronization2.

## Architecture

| Class | Purpose | RHI interface |
|-------|---------|---------------|
| `VulkanDevice` | Instance + physical device + logical device + queues | `IRHIDevice` |
| `VulkanBuffer` | `VkBuffer` + `VkDeviceMemory`, optional persistent mapping | `IRHIBuffer` |
| `VulkanTexture` | `VkImage` + `VkImageView`, tracks current layout | `IRHITexture` |
| `VulkanShader` | `VkShaderModule` compiled from SPIR-V | `IRHIShader` |
| `VulkanSampler` | `VkSampler` | `IRHISampler` |
| `VulkanPipelineState` | `VkPipeline` + `VkPipelineLayout` | `IRHIPipelineState` |
| `VulkanSwapChain` | `VkSurfaceKHR` + `VkSwapchainKHR` | `IRHISwapChain` |
| `VulkanCommandList` | Per-frame primary `VkCommandBuffer` | `IRHICommandList` |
| `VulkanDescriptorCache` | LRU-cached `VkDescriptorSet` layouts | internal |

### Queue families

Queue selection picks the best-fit families from the platform:

```cpp
struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;   // graphics + present
    std::optional<uint32_t> computeFamily;    // async compute (optional)
    std::optional<uint32_t> transferFamily;   // DMA copies (optional)
    std::optional<uint32_t> presentFamily;
    bool IsComplete() const;
};
```

Separate compute and transfer queues are used when the adapter exposes them; otherwise the graphics queue handles all three.

## Features

- **Vulkan 1.4 dynamic rendering** — no `VkRenderPass` objects; pipelines bind color / depth formats inline via `VK_KHR_dynamic_rendering` (core in 1.3+).
- **Push descriptors** — frequent CBV/SRV bindings skip descriptor-set allocation via `VK_KHR_push_descriptor`.
- **Timeline semaphores** — unified CPU/GPU synchronization primitive (`VK_KHR_timeline_semaphore`). Used for async compute and frame pacing.
- **Synchronization2** — coarse-grained `VkPipelineStageFlags2` and image-barrier builders.
- **VMA / explicit memory** — currently plain `vkAllocateMemory` with a per-heap budget cache; the VMA port is on the Tier-2 roadmap.
- **Descriptor cache** — `VulkanDescriptorCache` memoizes `VkDescriptorSetLayout` so render passes don't rebuild them every frame.
- **Validation layer** — Debug builds automatically enable `VK_LAYER_KHRONOS_validation`; the `VK_EXT_debug_utils` callback routes messages through `Spark::Logger`.
- **Lavapipe development route** — Linux development tests may use Mesa Lavapipe with explicitly configured software-ICD and display settings. This is not a generic fallback or a support/certification claim. See [Cross-Compilation: Wine Testing](../platform/Cross-Compilation-Wine-Testing.md).

## Capability detection

```cpp
auto& caps = device->GetCapabilities();
caps.rayTracingSupport         // VK_KHR_ray_tracing_pipeline
caps.meshShaderSupport         // VK_EXT_mesh_shader
caps.bindlessResourceSupport   // VK_EXT_descriptor_indexing + update-after-bind
caps.conservativeRasterSupport // VK_EXT_conservative_rasterization
```

## Factory registration

```cpp
// RHIFactory.cpp
case GraphicsBackend::Vulkan:
    device = std::make_unique<Vulkan::VulkanDevice>();
    break;
```

On Linux, `RHIFactory::GetRecommendedBackend()` can select Vulkan from the compiled and available candidates. Runtime availability and failure handling are owned by `RHIBridge`, not by the factory selector.

## Known issues / scope

- **MoltenVK macOS development path** — no certified surface or packaged-runtime proof exists; the native [Metal backend](Metal-Backend.md) is also partial and blocked.
- **Mesh shaders** — plumbed through `VK_EXT_mesh_shader`, requires recent Mesa (RADV) or NVIDIA drivers.
- **DXR equivalent** — implemented via `VK_KHR_ray_tracing_pipeline` but currently flagged experimental. See [DXR Raytracing](DXR-Raytracing.md) for the D3D12 sibling.
- **Release evidence** — full production-pass execution, GPU readback, shader-toolchain, and supported-driver evidence remain incomplete.

## Related

- [RHI Abstraction Layer](RHI-Abstraction-Layer.md) — shared interface.
- [D3D12 Backend](D3D12-Backend.md) — Windows modern alternative.
- [OpenGL Backend](OpenGL-Backend.md) — experimental development alternative when compiled.
- [Cross-Compilation: Wine Testing](../platform/Cross-Compilation-Wine-Testing.md) — running D3D11 builds under Wine + DXVK (Vulkan).
