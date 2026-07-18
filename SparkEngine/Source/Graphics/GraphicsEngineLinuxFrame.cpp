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

    // Phase U: pump the Spark::Graphics::ShaderHotReload singleton each
    // frame so runtime shader hot-reload runs on Linux and headless
    // builds. The singleton has its own poll-interval gating (default
    // 0.5 s) so a fixed nominal delta is both safe and cheap. Previously
    // guarded by `if (m_shader)`, but m_shader is never instantiated on
    // either platform — calling the singleton directly bypasses the
    // dead member and actually runs the file watcher.
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
        Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
        Spark::RHI::IRHITexture* depthBuffer = rhi.bridge.GetDepthBuffer();

        if (backBuffer)
        {
            cmd->SetRenderTargets(&backBuffer, 1, depthBuffer);
            cmd->ClearRenderTarget(backBuffer, m_settings.clearColor);
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

    // Post-processing runs after scene rendering and before Present.
    // Without this call, all 16 effect passes (Bloom, DoF, Tonemapping,
    // ColorGrading, etc.) were silently skipped on Linux.
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
        obj->Render(viewMatrix, projMatrix);
        m_statistics.drawCalls++;
    }

    m_statistics.visibleObjects = visibleCount;
    m_statistics.culledObjects = m_statistics.totalObjects - visibleCount;

    cmd->EndEvent();
}

// SubmitMeshForRendering lives in the shared GraphicsEngineSubmit.cpp so
// Linux/macOS builds can drive the draw list too. ProcessDrawList (D3D11-
// specific) stays Windows-only; on Linux/macOS the draw list accumulates
// but is consumed by the RHI bridge path or the Metal RT scene feeder
// (`Graphics/HybridRT/RTSceneFeeder.h`).

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
