/**
 * @file GraphicsEngineLinuxRenderTargets.cpp
 * @brief Platform render target helpers (Linux/macOS) for GraphicsEngine
 *
 * Create/destroy the GBuffer + HDR + Depth textures and keep the RHI bridge's
 * render-target registry in sync. On Windows this is the D3D11 ComPtr path in
 * `CreateAdvancedRenderTargets`; here it uses `RHIBridge::CreateTexture2D` /
 * `CreateDepthBuffer` so every backend (Vulkan, OpenGL, Metal, NullRHI) gets
 * the same slot layout.
 *
 * Free functions in `Spark::Graphics::Detail` (not `GraphicsEngine` members)
 * so `GraphicsEngine.h` doesn't need a new declaration — the Linux state they
 * touch lives in `LinuxRHIState`, and they're declared in the split-internal
 * `GraphicsEngineRHI.h` for the lifecycle TU (GraphicsEngineLinux.cpp:
 * Initialize / Shutdown / Resize) to call.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngineRHI.h"
#include "RHI/RHI.h"

namespace Spark::Graphics::Detail
{
    void CreatePlatformRenderTargets(uint32_t width, uint32_t height)
    {
        auto& rhi = GetRHI();
        if (!rhi.initialized)
            return;

        auto* device = rhi.bridge.GetDevice();
        if (!device)
            return;

        using Slot = Spark::RHI::RHIBridge::RenderTargetSlot;
        using Spark::RHI::PixelFormat;
        using Spark::RHI::RHITextureUsage;

        // GBuffer[0]: Albedo — RGBA8 color write + SRV read.
        rhi.gBufferAlbedo = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R8G8B8A8_UNORM);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferAlbedo, rhi.gBufferAlbedo.get());

        // GBuffer[1]: Normals — R16G16B16A16F matches the Windows
        // `WrapNativeTexture` format in GraphicsEngineWindows::AcquireHybridRTBindings
        // so downstream shaders don't need a platform-specific variant.
        rhi.gBufferNormals = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R16G16B16A16_FLOAT);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferNormals, rhi.gBufferNormals.get());

        // GBuffer[2]: Material (roughness / metallic / AO / reserved in RGBA8).
        rhi.gBufferMaterial = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R8G8B8A8_UNORM);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMaterial, rhi.gBufferMaterial.get());

        // GBuffer[3]: Motion vectors in RG16F. Unused by default on NullRHI
        // but keeping the slot populated means TAA / temporal passes can look
        // it up without extra null checks.
        rhi.gBufferMotion = rhi.bridge.CreateRenderTarget(width, height, PixelFormat::R16G16_FLOAT);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMotion, rhi.gBufferMotion.get());

        // Depth stencil — RHI-side convenience helper sets the DepthStencil
        // usage flag for us. Must match the swapchain's depth format on real
        // backends; NullRHI accepts anything.
        rhi.depthStencil = rhi.bridge.CreateDepthBuffer(width, height, PixelFormat::D24_UNORM_S8_UINT);
        rhi.bridge.RegisterRenderTarget(Slot::DepthStencil, rhi.depthStencil.get());

        // HDR lighting — R16G16B16A16F with RenderTarget|ShaderResource|UnorderedAccess.
        // The UAV flag is what lets the HybridRT compute pass write into it;
        // the default `CreateRenderTarget` helper only asks for RT|SRV, so we
        // go through `CreateTexture2D` with explicit usage flags.
        rhi.hdrLighting = rhi.bridge.CreateTexture2D(width, height, PixelFormat::R16G16B16A16_FLOAT,
                                                     RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource |
                                                         RHITextureUsage::UnorderedAccess,
                                                     nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::HDRLighting, rhi.hdrLighting.get());
    }

    void ReleasePlatformRenderTargets()
    {
        auto& rhi = GetRHI();
        if (!rhi.initialized)
            return;

        using Slot = Spark::RHI::RHIBridge::RenderTargetSlot;
        rhi.bridge.RegisterRenderTarget(Slot::GBufferAlbedo, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferNormals, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMaterial, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::GBufferMotion, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::DepthStencil, nullptr);
        rhi.bridge.RegisterRenderTarget(Slot::HDRLighting, nullptr);

        rhi.gBufferAlbedo.reset();
        rhi.gBufferNormals.reset();
        rhi.gBufferMaterial.reset();
        rhi.gBufferMotion.reset();
        rhi.depthStencil.reset();
        rhi.hdrLighting.reset();
    }
} // namespace Spark::Graphics::Detail

#endif // !SPARK_PLATFORM_WINDOWS
