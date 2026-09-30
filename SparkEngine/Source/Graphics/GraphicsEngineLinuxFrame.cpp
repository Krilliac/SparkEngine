/**
 * @file GraphicsEngineLinuxFrame.cpp
 * @brief Linux RHI-bridge per-frame rendering path for GraphicsEngine
 *
 * BeginFrame / EndFrame / RenderScene / AcquireHybridRTBindings split out of
 * GraphicsEngineLinux.cpp (which keeps lifecycle: Initialize / Shutdown /
 * Resize). Windows counterpart lives in GraphicsEngineWindows.cpp.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"
#include "VRAMBudgetMonitor.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
// Phase U: activated Tier 2 graphics orphan — process-wide shader file
// watcher. Pumped from the Linux BeginFrame so headless / RHI builds
// share the same per-frame hot-reload poll that the Windows branch gets
// via Shader::HotReloadShaders.
#include "ShaderHotReload.h"
#include "../Game/GameObject.h"
#include "RHI/RHI.h"
#include <chrono>

using namespace Spark::Graphics::Detail;

namespace
{
    /**
     * Whether this frame's scene renders into hdrLighting for the tone-mapping pass. A frame
     * asks for it by enabling PostProcessPass::Tonemapping; the pass runs only the variant the
     * shipped PostProcess shader implements (ACES, contrast 1) and only with its resources.
     * Otherwise the frame renders straight to the back buffer and, if tone mapping was asked
     * for, is counted as rejected instead of being reported as tone-mapped.
     */
    bool RouteSceneThroughTonemap(LinuxRHIState& rhi, const Spark::Graphics::PostProcessingPipeline* post,
                                  const Spark::RHI::IRHITexture* backBuffer)
    {
        using Spark::Graphics::PostProcessPass;
        using Spark::Graphics::TonemapOperator;
        if (!post || !post->IsInitialized() || !post->IsEffectEnabled(PostProcessPass::Tonemapping))
        {
            return false;
        }

        const auto& settings = post->GetTonemappingSettings();
        const char* reason = nullptr;
        if (settings.op != TonemapOperator::ACES || settings.contrast != 1.0f)
        {
            reason = "only the ACES operator with contrast 1 is implemented by the shipped PostProcess shader";
        }
        else if (!rhi.tonemap.pipeline || !rhi.basicForward.hdrPipeline || !rhi.hdrLighting || !backBuffer)
        {
            reason = "the tone-mapping pass resources are unavailable";
        }
        else if (rhi.hdrLighting->GetWidth() != backBuffer->GetWidth() ||
                 rhi.hdrLighting->GetHeight() != backBuffer->GetHeight())
        {
            reason = "the HDR scene target does not match the back buffer size";
        }

        if (reason != nullptr)
        {
            ++rhi.tonemap.rejectedFrames;
            SPARK_LOG_ONCE(Spark::LogLevel::Error, Spark::LogCategory::Graphics,
                           "Tone mapping requested but not performed: %s", reason);
            return false;
        }
        return true;
    }
} // namespace

// ============================================================================
// Frame Management
// ============================================================================

void GraphicsEngine::BeginFrame()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    bool expected = false;
    if (!m_frameInProgress.compare_exchange_strong(expected, true))
        return;

    rhi.frameStart = std::chrono::high_resolution_clock::now();

    rhi.bridge.BeginFrame();

    // Update VRAM budget monitor (lightweight query)
    if (m_vramBudgetMonitor)
        m_vramBudgetMonitor->Update();

    // Pump the Spark::Graphics::ShaderHotReload singleton each frame. The
    // singleton has its own poll-interval gating (default 0.5 s) so a fixed
    // nominal delta is both safe and cheap. Update() returns immediately
    // until ShaderHotReload::Initialize() enables the watcher, which no
    // production code does yet — see the shader pipeline notes.
    Spark::Graphics::ShaderHotReload::GetInstance().Update(1.0f / 60.0f);

    // Per-frame subsystem updates. These advance async load queues,
    // tile-binning counters, shadow cache frame state, and temporal
    // effect history. Without them, the subsystems silently stall.
    const float kNominalDeltaTime = 1.0f / 60.0f;

    if (m_assetPipeline)
        m_assetPipeline->Update(kNominalDeltaTime);

    if (m_lightingSystem)
    {
        // Identity view/proj in headless mode — the lighting system reads
        // the view matrix only to extract camera position from its inverse.
        DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
        m_lightingSystem->Update(kNominalDeltaTime, identity, identity);
    }

    if (m_temporalEffects)
        m_temporalEffects->Update(kNominalDeltaTime);

    // Clear the back buffer
    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (cmd)
    {
        // The frame records into the device's immediate list, which EndFrame closes and
        // submits. A Vulkan command buffer must be begun before anything is recorded into it
        // (EndFrame submits nothing that was never begun), so the frame opens the list here.
        // OpenGL executes immediately and NullRHI only resets its per-list counters.
        cmd->Begin();

        Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
        Spark::RHI::IRHITexture* depthBuffer = rhi.bridge.GetDepthBuffer();

        // With tone mapping the scene renders into the HDR target and RenderPostProcessing
        // resolves it into the back buffer; the back buffer is still cleared so a frame that
        // never reaches RenderScene presents the clear colour, not stale contents.
        rhi.sceneTarget =
            RouteSceneThroughTonemap(rhi, m_postProcessing.get(), backBuffer) ? rhi.hdrLighting.get() : backBuffer;
        if (backBuffer)
        {
            cmd->ClearRenderTarget(backBuffer, m_settings.clearColor);
        }
        if (rhi.sceneTarget)
        {
            cmd->SetRenderTargets(&rhi.sceneTarget, 1, depthBuffer);
            if (rhi.sceneTarget != backBuffer)
            {
                cmd->ClearRenderTarget(rhi.sceneTarget, m_settings.clearColor);
            }
        }
        if (depthBuffer)
        {
            cmd->ClearDepthStencil(depthBuffer, 1.0f, 0);
        }

        // Set viewport
        Spark::RHI::RHIViewport vp;
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(m_width);
        vp.height = static_cast<float>(m_height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        cmd->SetViewport(vp);

        Spark::RHI::RHIScissorRect sr;
        sr.left = 0;
        sr.top = 0;
        sr.right = static_cast<int32_t>(m_width);
        sr.bottom = static_cast<int32_t>(m_height);
        cmd->SetScissorRect(sr);
    }

    m_statistics.drawCalls = 0;
    m_statistics.triangles = 0;
    m_statistics.vertices = 0;
}

void GraphicsEngine::EndFrame()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;
    if (!m_frameInProgress.load())
        return;

    // Advances the PostProcessingPipeline's CPU-side state (volumes, timers) once per frame.
    // Its effect shaders are D3D11-only, so it records no GPU pass here; the Linux RHI
    // tone-mapping pass is recorded by RenderScene through RenderPostProcessing.
    if (m_postProcessing && m_postProcessing->IsInitialized())
    {
        m_postProcessing->Process(1.0f / 60.0f);
    }

    rhi.bridge.EndFrame();
    rhi.bridge.Present(m_settings.vsync);

    // Timing
    auto now = std::chrono::high_resolution_clock::now();
    float frameDelta = std::chrono::duration<float, std::milli>(now - rhi.frameStart).count();
    m_statistics.frameTime = frameDelta;
    m_statistics.cpuTime = frameDelta;

    // FPS calculation (rolling window)
    rhi.accumulatedTime += frameDelta;
    rhi.frameCount++;
    if (rhi.accumulatedTime >= 1000.0f)
    {
        rhi.measuredFps = rhi.frameCount;
        m_statistics.fps = rhi.measuredFps;
        rhi.frameCount = 0;
        rhi.accumulatedTime = 0.0f;
    }

    // Pull RHI statistics
    const auto& rhiStats = rhi.bridge.GetFrameStatistics();
    m_statistics.drawCalls += rhiStats.drawCalls;
    m_statistics.triangles += rhiStats.trianglesRendered;
    m_statistics.vertices += rhiStats.verticesProcessed;
    m_statistics.textureBinds = rhiStats.textureBinds;
    m_statistics.gpuTime = rhiStats.gpuFrameTime;
    m_statistics.totalGPUMemory = rhiStats.gpuMemoryUsed;

    m_frameInProgress.store(false);
}

// ============================================================================
// RenderScene
// ============================================================================

void GraphicsEngine::RenderScene(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                 const std::vector<GameObject*>& objects)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("RenderScene");

    m_statistics.totalObjects = static_cast<uint32_t>(objects.size());
    uint32_t visibleCount = 0;

    for (auto* obj : objects)
    {
        if (!obj)
            continue;
        if (!obj->IsActive() || !obj->IsVisible())
            continue;

        visibleCount++;
        // Draws are counted by the RHI backend when recorded (folded in by
        // EndFrame); a visible object whose Render records nothing is not a draw.
        obj->Render(viewMatrix, projMatrix);
    }

    m_statistics.visibleObjects = visibleCount;
    m_statistics.culledObjects = m_statistics.totalObjects - visibleCount;

    // Draw and drain the ECS draw list (SubmitMeshForRendering) through the forward pass, as
    // the Windows RenderScene does. Without the drain the list grows every frame.
    ProcessDrawList(viewMatrix, projMatrix);

    // Post-processing after the scene, as the Windows RenderScene does: resolves the HDR scene
    // target into the back buffer when BeginFrame routed this frame through tone mapping.
    RenderPostProcessing();

    cmd->EndEvent();
}

// SubmitMeshForRendering and the non-Windows ProcessDrawList live in the
// shared GraphicsEngineSubmit.cpp.

// ============================================================================
// HybridRT GBuffer binding — Linux/macOS
// ============================================================================
// Reads the GBuffer/HDR textures from the RHI bridge's render-target
// registry. The rendering layer is expected to have registered them
// after creation (matched slot numbers in RenderTargetSlot). Anything
// still unregistered stays nullptr; DispatchHybridRTPass skips when
// IsReady() returns false, so partial registration degrades cleanly.

Spark::Graphics::HybridRTBindings GraphicsEngine::AcquireHybridRTBindings()
{
    Spark::Graphics::HybridRTBindings bindings;

    // The Linux/macOS RHI bridge lives in the LinuxRHIState singleton — the
    // GraphicsEngine::m_rhiBridge member is never populated on this branch
    // (it's a Windows-only alias). Source the bridge directly from GetRHI()
    // so unregistered slots degrade to nullptr and DispatchHybridRTPass's
    // `IsReady()` guard skips the pass cleanly.
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return bindings;

    using Slot = Spark::RHI::RHIBridge::RenderTargetSlot;
    bindings.normals = rhi.bridge.GetRenderTarget(Slot::GBufferNormals);
    bindings.depth = rhi.bridge.GetRenderTarget(Slot::DepthStencil);
    bindings.albedo = rhi.bridge.GetRenderTarget(Slot::GBufferAlbedo);
    bindings.lighting = rhi.bridge.GetRenderTarget(Slot::HDRLighting);
    return bindings;
}

#endif // !SPARK_PLATFORM_WINDOWS
