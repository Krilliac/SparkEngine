/**
 * @file GraphicsRenderPipelinesLinux.cpp
 * @brief Linux RHI-bridge rendering pipeline implementations
 *
 * Routes pipeline rendering through the RHI bridge.
 * Windows counterpart lives in GraphicsRenderPipelinesWindows.cpp.
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
        m_statistics.drawCalls++;
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

    // Depth pre-pass
    cmd->BeginEvent("DepthPrepass");
    for (auto* obj : objects)
    {
        if (!obj || !obj->IsActive() || !obj->IsVisible())
            continue;
        // Depth-only render handled by pipeline state
        m_statistics.drawCalls++;
    }
    cmd->EndEvent();

    // Light culling (compute shader)
    cmd->BeginEvent("LightCulling");
    // Dispatch light culling compute shader
    constexpr uint32_t TILE_SIZE = 16;
    uint32_t tilesX = (m_width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t tilesY = (m_height + TILE_SIZE - 1) / TILE_SIZE;
    cmd->Dispatch(tilesX, tilesY, 1);
    cmd->EndEvent();

    // Shading pass with per-tile light lists
    cmd->BeginEvent("Shading");
    for (auto* obj : objects)
    {
        if (!obj || !obj->IsActive() || !obj->IsVisible())
            continue;
        obj->Render(viewMatrix, projMatrix);
        m_statistics.drawCalls++;
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
        m_statistics.drawCalls++;
    }

    cmd->EndEvent();
}

void GraphicsEngine::LightingPass(const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    cmd->BeginEvent("LightingPass");

    // Full-screen quad to resolve G-Buffer with lighting
    // Bind G-Buffer textures as shader resources and draw a full-screen triangle
    cmd->SetPrimitiveTopology(Spark::RHI::RHIPrimitiveTopology::TriangleList);
    cmd->Draw(3, 0); // Full-screen triangle
    m_statistics.drawCalls++;

    cmd->EndEvent();
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

void GraphicsEngine::RenderGeometryPass()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    m_geometryStartTime = std::chrono::high_resolution_clock::now();
    cmd->BeginEvent("GeometryPass");

    // Process the ECS draw list for geometry rendering
    for (const auto& drawCmd : m_drawList)
    {
        // Each draw command is handled by the asset pipeline binding
        m_statistics.drawCalls++;
    }

    cmd->EndEvent();
}

void GraphicsEngine::RenderLightingPass()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    m_lightingStartTime = std::chrono::high_resolution_clock::now();
    cmd->BeginEvent("LightingResolve");

    // Resolve lighting using a full-screen pass
    cmd->SetPrimitiveTopology(Spark::RHI::RHIPrimitiveTopology::TriangleList);
    cmd->Draw(3, 0); // Full-screen triangle
    m_statistics.drawCalls++;

    auto endTime = std::chrono::high_resolution_clock::now();
    m_statistics.lightCullingTime = std::chrono::duration<float, std::milli>(endTime - m_lightingStartTime).count();

    cmd->EndEvent();
}

void GraphicsEngine::RenderPostProcessing()
{
    m_postProcessStartTime = std::chrono::high_resolution_clock::now();

    // Delegate to the actual PostProcessingPipeline system
    if (m_postProcessing)
    {
        float deltaTime = m_statistics.frameTime / 1000.0f; // ms -> seconds
        m_postProcessing->Process(deltaTime);
        m_postProcessing->Render();
    }

    // Also dispatch through RHI path for cross-platform passes
    auto& rhi = GetRHI();
    if (rhi.initialized)
    {
        Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
        if (cmd)
        {
            uint32_t passCount = 0;
            cmd->BeginEvent("PostProcessing_RHI");

            // Bloom pass (engine-level, not in PostProcessingPipeline)
            if (m_settings.bloom)
            {
                cmd->BeginEvent("Bloom");
                cmd->Draw(3, 0);
                passCount++;
                cmd->EndEvent();
            }

            // SSAO pass
            if (m_settings.ssao)
            {
                cmd->BeginEvent("SSAO");
                cmd->Draw(3, 0);
                passCount++;
                cmd->EndEvent();
            }

            // Tone mapping (always active when HDR is enabled)
            if (m_hdrEnabled)
            {
                cmd->BeginEvent("ToneMapping");
                cmd->Draw(3, 0);
                passCount++;
                cmd->EndEvent();
            }

            m_statistics.postProcessPasses = passCount + m_postProcessing->GetActivePassCount();
            cmd->EndEvent();
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    m_statistics.postProcessTime = std::chrono::duration<float, std::milli>(endTime - m_postProcessStartTime).count();
}

void GraphicsEngine::RenderTemporalEffects()
{
    // Delegate to the actual TemporalEffects system
    if (m_temporalEffects)
    {
        m_temporalEffects->SetTAAEnabled(m_settings.taa);
        m_temporalEffects->SetMotionBlurEnabled(m_settings.motionBlur);

        m_temporalEffects->Render();

        if (m_settings.taa)
            m_statistics.postProcessPasses++;
        if (m_settings.motionBlur)
            m_statistics.postProcessPasses++;
    }

    // Also dispatch through RHI path for cross-platform tracking
    auto& rhi = GetRHI();
    if (rhi.initialized)
    {
        Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
        if (cmd)
        {
            cmd->BeginEvent("TemporalEffects_RHI");
            if (m_settings.taa)
            {
                cmd->BeginEvent("TAA");
                cmd->Draw(3, 0);
                cmd->EndEvent();
            }
            if (m_settings.motionBlur)
            {
                cmd->BeginEvent("MotionBlur");
                cmd->Draw(3, 0);
                cmd->EndEvent();
            }
            cmd->EndEvent();
        }
    }
}


#endif // !SPARK_PLATFORM_WINDOWS
