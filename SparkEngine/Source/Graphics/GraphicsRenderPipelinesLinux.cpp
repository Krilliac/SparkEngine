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
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
#include "../Game/GameObject.h"

#include <chrono>
#include <cmath>
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
        // Frustum culling via view-projection matrix
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, viewProj);

        // Extract 6 frustum planes from VP matrix (Griggs-Hartmann method)
        float planes[6][4];
        // Left:   row3 + row0
        planes[0][0] = vp.m[0][3] + vp.m[0][0];
        planes[0][1] = vp.m[1][3] + vp.m[1][0];
        planes[0][2] = vp.m[2][3] + vp.m[2][0];
        planes[0][3] = vp.m[3][3] + vp.m[3][0];
        // Right:  row3 - row0
        planes[1][0] = vp.m[0][3] - vp.m[0][0];
        planes[1][1] = vp.m[1][3] - vp.m[1][0];
        planes[1][2] = vp.m[2][3] - vp.m[2][0];
        planes[1][3] = vp.m[3][3] - vp.m[3][0];
        // Bottom: row3 + row1
        planes[2][0] = vp.m[0][3] + vp.m[0][1];
        planes[2][1] = vp.m[1][3] + vp.m[1][1];
        planes[2][2] = vp.m[2][3] + vp.m[2][1];
        planes[2][3] = vp.m[3][3] + vp.m[3][1];
        // Top:    row3 - row1
        planes[3][0] = vp.m[0][3] - vp.m[0][1];
        planes[3][1] = vp.m[1][3] - vp.m[1][1];
        planes[3][2] = vp.m[2][3] - vp.m[2][1];
        planes[3][3] = vp.m[3][3] - vp.m[3][1];
        // Near:   row2
        planes[4][0] = vp.m[0][2];
        planes[4][1] = vp.m[1][2];
        planes[4][2] = vp.m[2][2];
        planes[4][3] = vp.m[3][2];
        // Far:    row3 - row2
        planes[5][0] = vp.m[0][3] - vp.m[0][2];
        planes[5][1] = vp.m[1][3] - vp.m[1][2];
        planes[5][2] = vp.m[2][3] - vp.m[2][2];
        planes[5][3] = vp.m[3][3] - vp.m[3][2];

        // Normalize planes
        for (auto& p : planes)
        {
            float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > 0.0f)
            {
                float inv = 1.0f / len;
                p[0] *= inv;
                p[1] *= inv;
                p[2] *= inv;
                p[3] *= inv;
            }
        }

        for (auto* obj : objects)
        {
            if (!obj || !obj->IsActive() || !obj->IsVisible())
                continue;

            // Sphere-based frustum test
            XMFLOAT3 pos = obj->GetPosition();
            constexpr float boundingRadius = 5.0f;
            bool visible = true;
            for (int i = 0; i < 6; ++i)
            {
                float dist = planes[i][0] * pos.x + planes[i][1] * pos.y + planes[i][2] * pos.z + planes[i][3];
                if (dist < -boundingRadius)
                {
                    visible = false;
                    break;
                }
            }
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

void GraphicsEngine::RenderPostProcessing()
{
    m_postProcessStartTime = std::chrono::high_resolution_clock::now();

    // PostProcessingPipeline is the only post-process path on Linux. Its pass
    // count includes only passes that actually executed (a bound shader and a
    // recorded draw); the engine-level Bloom/SSAO/tone-mapping settings have
    // no Linux RHI pipeline, so they add no passes of their own.
    uint32_t executedPasses = 0;
    if (m_postProcessing)
    {
        float deltaTime = m_statistics.frameTime / 1000.0f; // ms -> seconds
        m_postProcessing->Process(deltaTime);
        m_postProcessing->Render();
        executedPasses = static_cast<uint32_t>(m_postProcessing->GetActivePassCount());
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
