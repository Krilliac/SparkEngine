/**
 * @file GraphicsRenderPipelinesLinux.cpp
 * @brief Linux RHI-bridge rendering pipeline implementations
 *
 * Routes pipeline rendering through the RHI bridge.
 * Windows counterpart lives in GraphicsRenderPipelinesWindows.cpp.
 *
 * Statistics contract: these passes never increment
 * RenderStatistics::drawCalls themselves. Draws are counted by the active RHI
 * backend as they are recorded and folded in by EndFrame, so a pass that
 * records nothing reports nothing. None of these passes issues a draw or dispatch
 * without a pipeline bound for it.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "D3D11FrustumCulling.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
#include "../Game/GameObject.h"
#include "../Utils/LogMacros.h"

#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

using namespace DirectX;
using namespace Spark::Graphics::Detail;

// ============================================================================
// Rendering Pipelines — Linux/RHI
// ============================================================================

void GraphicsEngine::RenderForward(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                   const std::vector<GameObject*>& objects)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("ForwardPass");
    ApplyGraphicsState();

    for (auto* obj : objects)
    {
        if (!obj || !obj->IsActive() || !obj->IsVisible())
            continue;
        obj->Render(viewMatrix, projMatrix);
    }

    cmd->EndEvent();
}

void GraphicsEngine::RenderDeferred(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                    const std::vector<GameObject*>& objects)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("DeferredPass");

    // Geometry pass: fill G-Buffer
    FillGBuffer(objects, viewMatrix, projMatrix);

    // Lighting pass: resolve G-Buffer with lighting
    LightingPass(viewMatrix, projMatrix);

    // Phase 2.5: Hybrid ray tracing — matches the Windows pipeline shape.
    // On Linux/macOS, AcquireHybridRTBindings() currently returns empty
    // bindings so DispatchHybridRTPass is a no-op; HybridRTManager falls
    // through to SDFGI / software path. The call site is wired now so that
    // once the RHI bridge exposes GBuffer textures (phase 7 follow-up),
    // enabling hardware RT on macOS is a single-file change.
#ifdef SPARK_HYBRID_RT
    DispatchHybridRTPass(cmd, viewMatrix, projMatrix);
#endif

    cmd->EndEvent();
}

void GraphicsEngine::RenderForwardPlus(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                       const std::vector<GameObject*>& objects)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("ForwardPlusPass");

    // The Linux path has no depth-prepass or tiled light-culling compute
    // pipeline yet, so neither is recorded: an unbound Dispatch is not a
    // light-culling pass. Objects shade through their own Render path.
    cmd->BeginEvent("Shading");
    for (auto* obj : objects)
    {
        if (!obj || !obj->IsActive() || !obj->IsVisible())
            continue;
        obj->Render(viewMatrix, projMatrix);
    }
    cmd->EndEvent();

    cmd->EndEvent();
}

void GraphicsEngine::FillGBuffer(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix,
                                 const XMMATRIX& projMatrix)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("GBufferFill");

    for (auto* obj : objects)
    {
        if (!obj || !obj->IsActive() || !obj->IsVisible())
            continue;
        obj->Render(viewMatrix, projMatrix);
    }

    cmd->EndEvent();
}

void GraphicsEngine::LightingPass(const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/)
{
    // The Linux RHI path has no G-buffer lighting-resolve pipeline (shader,
    // G-buffer SRVs, HDR target) yet. A full-screen Draw(3, 0) with nothing
    // bound is not a lighting pass, so nothing is recorded and nothing is
    // counted until a real resolve pipeline is bound here.
}

void GraphicsEngine::CullObjects(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix,
                                 const XMMATRIX& projMatrix, std::vector<GameObject*>& visibleObjects)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    visibleObjects.clear();

    if (!m_settings.frustumCulling)
    {
        // No culling: pass all active/visible objects through
        for (auto* obj : objects)
        {
            if (obj && obj->IsActive() && obj->IsVisible())
                visibleObjects.push_back(obj);
        }
    }
    else
    {
        // DirectXMath uses row vectors, so extraction uses view-projection columns.
        const XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        const auto planes = Spark::Graphics::D3D11RenderMath::ExtractFrustumPlanes(viewProj);

        for (auto* obj : objects)
        {
            if (!obj || !obj->IsActive() || !obj->IsVisible())
                continue;

            // Sphere-based frustum test
            XMFLOAT3 pos = obj->GetPosition();
            constexpr float boundingRadius = 5.0f;
            const bool visible = Spark::Graphics::D3D11RenderMath::SphereIntersects(planes, pos, boundingRadius);
            if (visible)
                visibleObjects.push_back(obj);
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    m_statistics.cullingTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    m_statistics.totalObjects = static_cast<uint32_t>(objects.size());
    m_statistics.visibleObjects = static_cast<uint32_t>(visibleObjects.size());
    m_statistics.culledObjects = m_statistics.totalObjects - m_statistics.visibleObjects;
}

// ============================================================================
// Tone-mapping post pass — Linux/RHI
// ============================================================================

bool Spark::Graphics::Detail::CreateTonemapPass(LinuxRHIState& rhi, const Spark::RHI::RHIPipelineStateDesc& forwardDesc,
                                                Spark::RHI::IRHIShader* forwardVs, Spark::RHI::IRHIShader* forwardPs,
                                                bool headless)
{
    using Spark::RHI::PixelFormat;
    using Spark::RHI::RHIShaderStage;

    Spark::RHI::IRHIDevice* device = rhi.bridge.GetDevice();
    Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
    if (!device || !rhi.hdrLighting)
    {
        return false;
    }

    // The Linux RHI backends read GLSL (OpenGL) or its SPIR-V (Vulkan); there is no HLSL
    // version of these two stages, and no D3D backend on this path.
    rhi.bridge.RegisterShader("fullscreen_vs", RHIShaderStage::Vertex, "", "Shaders/GLSL/FullscreenQuad.glsl",
                              "Shaders/SPIRV/FullscreenQuad.vert.spv", "main");
    rhi.bridge.RegisterShader("post_tonemap_ps", RHIShaderStage::Pixel, "", "Shaders/GLSL/PostProcess.glsl",
                              "Shaders/SPIRV/PostProcess.frag.spv", "main");
    Spark::RHI::IRHIShader* vs = headless ? nullptr : rhi.bridge.GetShader("fullscreen_vs");
    Spark::RHI::IRHIShader* ps = headless ? nullptr : rhi.bridge.GetShader("post_tonemap_ps");
    if (!headless && (!vs || !ps))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load the tone-mapping shaders via RHI");
        return false;
    }

    Spark::RHI::RHIPipelineStateDesc hdrDesc = forwardDesc;
    hdrDesc.renderTargetFormats[0] = rhi.hdrLighting->GetFormat();
    hdrDesc.debugName = "BasicForwardPassHDR";
    auto hdrPipeline = device->CreatePipelineState(hdrDesc, forwardVs, forwardPs);

    // Full-screen triangle from the vertex index: no vertex input, no depth.
    Spark::RHI::RHIPipelineStateDesc desc;
    desc.numRenderTargets = 1;
    desc.renderTargetFormats[0] = backBuffer ? backBuffer->GetFormat() : PixelFormat::R8G8B8A8_UNORM;
    desc.depthStencilFormat = PixelFormat::Unknown;
    desc.depthStencil.depthEnable = false;
    desc.depthStencil.depthWrite = false;
    desc.rasterizer.cullMode = Spark::RHI::RHICullMode::None;
    desc.debugName = "TonemapPass";
    auto pipeline = device->CreatePipelineState(desc, vs, ps);

    auto constants = rhi.bridge.CreateConstantBuffer(sizeof(PostProcessConstants));
    auto sampler = rhi.bridge.CreateSamplerLinearClamp();
    if (!hdrPipeline || !pipeline || !constants || !sampler)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create the tone-mapping post pass resources");
        return false;
    }

    rhi.basicForward.hdrPipeline = std::move(hdrPipeline);
    rhi.tonemap.pipeline = std::move(pipeline);
    rhi.tonemap.constants = std::move(constants);
    rhi.tonemap.sampler = std::move(sampler);
    return true;
}

void GraphicsEngine::RenderPostProcessing()
{
    m_postProcessStartTime = std::chrono::high_resolution_clock::now();

    // The PostProcessingPipeline's CPU state advances once per frame in EndFrame; its effect
    // shaders are D3D11-only, so on this path it executes no pass itself. The one post pass
    // the Linux RHI path records is tone mapping (PostProcessPass::Tonemapping), and only for a
    // frame BeginFrame routed into the HDR scene target. The engine-level Bloom/SSAO settings
    // have no Linux RHI pipeline and add no passes of their own.
    uint32_t executedPasses = 0;
    auto& rhi = GetRHI();
    Spark::RHI::IRHICommandList* cmd = rhi.initialized ? rhi.bridge.GetCommandList() : nullptr;
    Spark::RHI::IRHIDevice* device = rhi.initialized ? rhi.bridge.GetDevice() : nullptr;
    Spark::RHI::IRHITexture* backBuffer = rhi.initialized ? rhi.bridge.GetBackBuffer() : nullptr;
    Spark::RHI::IRHITexture* hdrScene = rhi.hdrLighting.get();
    if (cmd && device && backBuffer && m_postProcessing && hdrScene && rhi.sceneTarget == hdrScene &&
        rhi.tonemap.pipeline)
    {
        const auto& settings = m_postProcessing->GetTonemappingSettings();
        PostProcessConstants constants{};
        constants.screenSize =
            XMFLOAT2(static_cast<float>(backBuffer->GetWidth()), static_cast<float>(backBuffer->GetHeight()));
        constants.invScreenSize = XMFLOAT2(1.0f / constants.screenSize.x, 1.0f / constants.screenSize.y);
        constants.exposure = settings.exposure;
        // The D3D11 tonemap pass writes the operator's output without a gamma curve.
        constants.gamma = 1.0f;
        constants.vignetteRadius = 0.75f;
        constants.saturation = settings.saturation;
        device->UpdateBuffer(rhi.tonemap.constants.get(), &constants, sizeof(constants));

        cmd->BeginEvent("Tonemapping");
        // Bind the input first: a sampled-image layout change cannot be recorded while the
        // back buffer is open for rendering (Vulkan dynamic rendering).
        cmd->SetShaderResource(Spark::RHI::RHIShaderStage::Pixel, 0, hdrScene);
        cmd->SetSampler(Spark::RHI::RHIShaderStage::Pixel, 0, rhi.tonemap.sampler.get());
        cmd->SetRenderTargets(&backBuffer, 1, nullptr);
        cmd->SetPipelineState(rhi.tonemap.pipeline.get());
        cmd->SetConstantBuffer(Spark::RHI::RHIShaderStage::Pixel, 1, rhi.tonemap.constants.get());
        cmd->Draw(3, 0);
        cmd->EndEvent();

        // Anything drawn after post-processing (debug overlays, UI) lands on the back buffer.
        cmd->SetRenderTargets(&backBuffer, 1, rhi.bridge.GetDepthBuffer());
        rhi.sceneTarget = backBuffer;
        executedPasses = 1;
    }
    m_statistics.postProcessPasses = executedPasses;

    auto endTime = std::chrono::high_resolution_clock::now();
    m_statistics.postProcessTime = std::chrono::duration<float, std::milli>(endTime - m_postProcessStartTime).count();
}

void GraphicsEngine::RenderTemporalEffects()
{
    // Keep TemporalEffects' CPU state (jitter, history) in step with the
    // settings. Its Linux Render() records no GPU work — TAA resolve and
    // motion blur exist only on the D3D11 path — so no pass is counted and
    // no RHI draw is issued for them here.
    if (m_temporalEffects)
    {
        m_temporalEffects->SetTAAEnabled(m_settings.taa);
        m_temporalEffects->SetMotionBlurEnabled(m_settings.motionBlur);
        m_temporalEffects->Render();
    }
}

#endif // !SPARK_PLATFORM_WINDOWS
