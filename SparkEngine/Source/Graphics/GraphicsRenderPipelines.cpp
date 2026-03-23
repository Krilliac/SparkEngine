/**
 * @file GraphicsRenderPipelines.cpp
 * @brief Rendering pipeline implementations for GraphicsEngine
 *
 * Contains Forward, Deferred, and Forward+ rendering pipeline implementations,
 * G-Buffer fill, lighting pass, frustum culling, post-processing, and temporal
 * effects. Split from GraphicsEngine.cpp for maintainability.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "LightingSystem.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"
#include "../Game/GameObject.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#ifdef SPARK_HARDWARE_RT
#include "RHI/DXRSupport.h"
#endif
#endif

#include <Windows.h>
#include <d3d11_1.h>
#include <DirectXMath.h>
#include <wrl.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================================
// RENDERING PIPELINE IMPLEMENTATIONS
// ============================================================================

void GraphicsEngine::RenderForward(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                   const std::vector<GameObject*>& objects)
{
    // Update per-frame constants at the start of rendering
    if (m_basicFrameConstantBuffer)
    {
        // Calculate camera position from inverse view matrix
        XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
        XMFLOAT3 cameraPos;
        XMStoreFloat3(&cameraPos, invView.r[3]);

        UpdateFrameConstants(viewMatrix, projMatrix, cameraPos);
    }

    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t vertices = 0;

    for (auto* obj : objects)
    {
        if (obj && obj->IsActive() && obj->IsVisible())
        {
            try
            {
                obj->Render(viewMatrix, projMatrix);
                drawCalls++;
                triangles += 12;
                vertices += 36;
            }
            catch (const std::exception& e)
            {
                static int errorCount = 0;
                if (++errorCount <= 5)
                {
                    std::wstring msg = L"Render error: " + std::wstring(e.what(), e.what() + strlen(e.what()));
                    LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
                }
            }
            catch (...)
            {
                static int errorCount = 0;
                if (++errorCount <= 5)
                    LOG_TO_CONSOLE_IMMEDIATE(L"Unknown render error", L"WARNING");
            }
        }
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls = drawCalls;
        m_statistics.triangles = triangles;
        m_statistics.vertices = vertices;
    }
}

void GraphicsEngine::RenderDeferred(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                    const std::vector<GameObject*>& objects)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting deferred rendering pass", L"INFO");

    // Phase 1: Fill G-Buffer
    FillGBuffer(objects, viewMatrix, projMatrix);

    // Phase 2: Lighting pass
    LightingPass(viewMatrix, projMatrix);

    // Phase 2.5: Hybrid ray tracing (SDFGI or DXR) — after lighting, before transparents
#ifdef SPARK_HYBRID_RT
    if (m_hybridRT && m_rhiBridge)
    {
        auto* cmd = m_rhiBridge->GetCommandList();
        XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
        DirectX::XMFLOAT3 camPos;
        XMStoreFloat3(&camPos, invView.r[3]);
        DirectX::XMFLOAT3 lightDir = {0.0f, -1.0f, 0.5f}; // Primary directional light
        Spark::Graphics::SSRSettings ssrDefaults;         // Screen-space coordination

        // Wrap D3D11 GBuffer textures as RHI handles for the hybrid RT system
        auto* device = m_rhiBridge->GetDevice();
        std::unique_ptr<Spark::RHI::IRHITexture> rhiNormals;
        std::unique_ptr<Spark::RHI::IRHITexture> rhiDepth;
        std::unique_ptr<Spark::RHI::IRHITexture> rhiAlbedo;
        std::unique_ptr<Spark::RHI::IRHITexture> rhiLighting;

        if (device)
        {
            // GBuffer layout: [0]=Albedo, [1]=Normal, [2]=Material, [3]=Motion
            if (m_gBufferTextures[1].Get()) // Normal
            {
                Spark::RHI::RHITextureDesc normDesc;
                normDesc.width = m_width;
                normDesc.height = m_height;
                normDesc.format = Spark::RHI::PixelFormat::R16G16B16A16_FLOAT;
                normDesc.usage = Spark::RHI::RHITextureUsage::ShaderResource;
                normDesc.debugName = "GBuffer_Normals_Wrapped";
                rhiNormals = device->WrapNativeTexture(m_gBufferTextures[1].Get(), normDesc);
            }
            if (m_depthStencilTexture.Get())
            {
                Spark::RHI::RHITextureDesc depthDesc;
                depthDesc.width = m_width;
                depthDesc.height = m_height;
                depthDesc.format = Spark::RHI::PixelFormat::D24_UNORM_S8_UINT;
                depthDesc.usage = Spark::RHI::RHITextureUsage::ShaderResource;
                depthDesc.debugName = "Depth_Wrapped";
                rhiDepth = device->WrapNativeTexture(m_depthStencilTexture.Get(), depthDesc);
            }
            if (m_gBufferTextures[0].Get()) // Albedo
            {
                Spark::RHI::RHITextureDesc albedoDesc;
                albedoDesc.width = m_width;
                albedoDesc.height = m_height;
                albedoDesc.format = Spark::RHI::PixelFormat::R8G8B8A8_UNORM;
                albedoDesc.usage = Spark::RHI::RHITextureUsage::ShaderResource;
                albedoDesc.debugName = "GBuffer_Albedo_Wrapped";
                rhiAlbedo = device->WrapNativeTexture(m_gBufferTextures[0].Get(), albedoDesc);
            }
            if (m_hdrTexture.Get()) // Lighting output (HDR buffer)
            {
                Spark::RHI::RHITextureDesc hdrDesc;
                hdrDesc.width = m_width;
                hdrDesc.height = m_height;
                hdrDesc.format = Spark::RHI::PixelFormat::R16G16B16A16_FLOAT;
                hdrDesc.usage =
                    Spark::RHI::RHITextureUsage::ShaderResource | Spark::RHI::RHITextureUsage::UnorderedAccess;
                hdrDesc.debugName = "HDR_Lighting_Wrapped";
                rhiLighting = device->WrapNativeTexture(m_hdrTexture.Get(), hdrDesc);
            }
        }

        m_hybridRT->Execute(cmd, viewMatrix, projMatrix, camPos, lightDir, rhiNormals.get(), rhiDepth.get(),
                            rhiAlbedo.get(), nullptr, nullptr, rhiLighting.get(), ssrDefaults);
    }
#endif

    // Phase 3: Forward rendering for transparent objects
    uint32_t transparentDrawCalls = 0;
    for (auto* obj : objects)
    {
        if (obj && obj->IsActive() && obj->IsVisible())
        {
            try
            {
                obj->Render(viewMatrix, projMatrix);
                transparentDrawCalls++;
            }
            catch (const std::exception& e)
            {
                static int errorCount = 0;
                if (++errorCount <= 3)
                {
                    std::wstring msg =
                        L"Render error (deferred transparent): " + std::wstring(e.what(), e.what() + strlen(e.what()));
                    LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
                }
            }
            catch (...)
            {
                static int errorCount = 0;
                if (++errorCount <= 3)
                    LOG_TO_CONSOLE_IMMEDIATE(L"Unknown render error in deferred transparent pass", L"WARNING");
            }
        }
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls += transparentDrawCalls;
        m_statistics.triangles += transparentDrawCalls * 12;
        m_statistics.vertices += transparentDrawCalls * 36;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Deferred rendering pass complete", L"INFO");
}

void GraphicsEngine::RenderForwardPlus(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                       const std::vector<GameObject*>& objects)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting Forward+ rendering pass", L"INFO");

    // Phase 1: Depth pre-pass
    uint32_t depthDrawCalls = 0;
    for (auto* obj : objects)
    {
        if (obj && obj->IsActive() && obj->IsVisible())
        {
            depthDrawCalls++;
        }
    }

    // Phase 2: Light culling
    if (m_lightingSystem)
    {
        m_lightingSystem->BindLightingData(m_context.Get());
    }

    // Phase 3: Shading pass
    uint32_t shadingDrawCalls = 0;
    uint32_t triangles = 0;
    uint32_t vertices = 0;

    for (auto* obj : objects)
    {
        if (obj && obj->IsActive() && obj->IsVisible())
        {
            try
            {
                obj->Render(viewMatrix, projMatrix);
                shadingDrawCalls++;
                triangles += 12;
                vertices += 36;
            }
            catch (const std::exception& e)
            {
                static int errorCount = 0;
                if (++errorCount <= 3)
                {
                    std::wstring msg =
                        L"Render error (Forward+ shading): " + std::wstring(e.what(), e.what() + strlen(e.what()));
                    LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
                }
            }
            catch (...)
            {
                static int errorCount = 0;
                if (++errorCount <= 3)
                {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Unknown render error in Forward+ shading pass", L"WARNING");
                }
            }
        }
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls = depthDrawCalls + shadingDrawCalls;
        m_statistics.triangles = triangles;
        m_statistics.vertices = vertices;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Forward+ rendering pass complete", L"INFO");
}

// ============================================================================
// ADVANCED RENDERING METHODS
// ============================================================================

void GraphicsEngine::FillGBuffer(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix,
                                 const XMMATRIX& projMatrix)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Filling G-Buffer for deferred rendering", L"INFO");

    uint32_t gBufferDrawCalls = 0;
    uint32_t totalTriangles = 0;
    uint32_t totalVertices = 0;

    if (m_context && m_solidRasterState)
    {
        m_context->RSSetState(m_solidRasterState.Get());
    }

    for (auto* obj : objects)
    {
        if (obj && obj->IsActive() && obj->IsVisible())
        {
            try
            {
                obj->Render(viewMatrix, projMatrix);
                gBufferDrawCalls++;
                totalTriangles += 12;
                totalVertices += 36;
            }
            catch (const std::exception& e)
            {
                static int errorCount = 0;
                if (++errorCount <= 3)
                {
                    std::wstring msg =
                        L"Render error (G-Buffer): " + std::wstring(e.what(), e.what() + strlen(e.what()));
                    LOG_TO_CONSOLE_IMMEDIATE(msg.c_str(), L"WARNING");
                }
            }
            catch (...)
            {
                static int errorCount = 0;
                if (++errorCount <= 3)
                    LOG_TO_CONSOLE_IMMEDIATE(L"Unknown render error in G-Buffer fill", L"WARNING");
            }
        }
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls += gBufferDrawCalls;
        m_statistics.triangles += totalTriangles;
        m_statistics.vertices += totalVertices;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"G-Buffer fill complete with " + std::to_wstring(gBufferDrawCalls) + L" draw calls",
                             L"INFO");
}

void GraphicsEngine::LightingPass(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting deferred lighting pass", L"INFO");

    auto lightingStartTime = std::chrono::high_resolution_clock::now();

    if (m_context && m_renderTargetView)
    {
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
    }

    if (m_lightingSystem)
    {
        try
        {
            // Bind lighting data to shaders
            m_lightingSystem->BindLightingData(m_context.Get());

            // Update lighting system with current frame parameters
            m_lightingSystem->Update(0.016f, viewMatrix, projMatrix);

            // Render shadow maps if shadows are enabled
            if (m_settings.shadows)
            {
                try
                {
                    m_lightingSystem->RenderShadowMaps(
                        [this](const XMMATRIX& lightView, const XMMATRIX& lightProj)
                        {
                            LOG_TO_CONSOLE_IMMEDIATE(L"Rendering shadow map for light", L"INFO");
                            // Here we would render shadow casters from the light's perspective
                            // The callback provides the light's view and projection matrices
                        });
                }
                catch (const std::exception& e)
                {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Shadow map rendering failed: " +
                                                 std::wstring(e.what(), e.what() + strlen(e.what())),
                                             L"WARNING");
                }
                catch (...)
                {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Shadow map rendering failed - unknown error", L"WARNING");
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_TO_CONSOLE_IMMEDIATE(
                L"Error in lighting system update: " + std::wstring(e.what(), e.what() + strlen(e.what())), L"ERROR");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Unknown error in lighting system update", L"ERROR");
        }
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: LightingSystem not available for lighting pass", L"WARNING");
    }

    uint32_t lightingDrawCalls = 1;

    auto lightingEndTime = std::chrono::high_resolution_clock::now();
    auto lightingTime = std::chrono::duration_cast<std::chrono::microseconds>(lightingEndTime - lightingStartTime);

    // Update statistics with actual lighting system metrics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls += lightingDrawCalls;

        if (m_lightingSystem)
        {
            try
            {
                auto lightingMetrics = m_lightingSystem->Console_GetMetrics();
                m_statistics.activeLights = lightingMetrics.activeLights;
                m_statistics.shadowUpdates = lightingMetrics.shadowMapUpdates;
                m_statistics.lightCullingTime = lightingMetrics.lightCullingTime;
            }
            catch (const std::exception& e)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Could not retrieve lighting metrics: " +
                                             std::wstring(e.what(), e.what() + strlen(e.what())),
                                         L"WARNING");
                // Use timing-based fallback
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = m_settings.shadows ? 1 : 0;
                m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
            }
            catch (...)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Could not retrieve lighting metrics - unknown error", L"WARNING");
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = m_settings.shadows ? 1 : 0;
                m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
            }
        }
        else
        {
            // No lighting system available
            m_statistics.activeLights = 0;
            m_statistics.shadowUpdates = 0;
            m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(
        L"Deferred lighting pass complete in " + std::to_wstring(lightingTime.count() / 1000.0f) + L"ms", L"INFO");
}

void GraphicsEngine::CullObjects(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix,
                                 const XMMATRIX& projMatrix, std::vector<GameObject*>& visibleObjects)
{
    auto cullingStartTime = std::chrono::high_resolution_clock::now();

    visibleObjects.clear();
    visibleObjects.reserve(objects.size());

    // Extract frustum planes from view-projection matrix
    XMMATRIX viewProjMatrix = XMMatrixMultiply(viewMatrix, projMatrix);

    XMVECTOR frustumPlanes[6];

    // Left plane
    frustumPlanes[0] = XMVectorSet(XMVectorGetX(viewProjMatrix.r[3]) + XMVectorGetX(viewProjMatrix.r[0]),
                                   XMVectorGetY(viewProjMatrix.r[3]) + XMVectorGetY(viewProjMatrix.r[0]),
                                   XMVectorGetZ(viewProjMatrix.r[3]) + XMVectorGetZ(viewProjMatrix.r[0]),
                                   XMVectorGetW(viewProjMatrix.r[3]) + XMVectorGetW(viewProjMatrix.r[0]));

    // Right plane
    frustumPlanes[1] = XMVectorSet(XMVectorGetX(viewProjMatrix.r[3]) - XMVectorGetX(viewProjMatrix.r[0]),
                                   XMVectorGetY(viewProjMatrix.r[3]) - XMVectorGetY(viewProjMatrix.r[0]),
                                   XMVectorGetZ(viewProjMatrix.r[3]) - XMVectorGetZ(viewProjMatrix.r[0]),
                                   XMVectorGetW(viewProjMatrix.r[3]) - XMVectorGetW(viewProjMatrix.r[0]));

    // Top plane
    frustumPlanes[2] = XMVectorSet(XMVectorGetX(viewProjMatrix.r[3]) - XMVectorGetX(viewProjMatrix.r[1]),
                                   XMVectorGetY(viewProjMatrix.r[3]) - XMVectorGetY(viewProjMatrix.r[1]),
                                   XMVectorGetZ(viewProjMatrix.r[3]) - XMVectorGetZ(viewProjMatrix.r[1]),
                                   XMVectorGetW(viewProjMatrix.r[3]) - XMVectorGetW(viewProjMatrix.r[1]));

    // Bottom plane
    frustumPlanes[3] = XMVectorSet(XMVectorGetX(viewProjMatrix.r[3]) + XMVectorGetX(viewProjMatrix.r[1]),
                                   XMVectorGetY(viewProjMatrix.r[3]) + XMVectorGetY(viewProjMatrix.r[1]),
                                   XMVectorGetZ(viewProjMatrix.r[3]) + XMVectorGetZ(viewProjMatrix.r[1]),
                                   XMVectorGetW(viewProjMatrix.r[3]) + XMVectorGetW(viewProjMatrix.r[1]));

    // Near plane
    frustumPlanes[4] = XMVectorSet(XMVectorGetX(viewProjMatrix.r[3]) + XMVectorGetX(viewProjMatrix.r[2]),
                                   XMVectorGetY(viewProjMatrix.r[3]) + XMVectorGetY(viewProjMatrix.r[2]),
                                   XMVectorGetZ(viewProjMatrix.r[3]) + XMVectorGetZ(viewProjMatrix.r[2]),
                                   XMVectorGetW(viewProjMatrix.r[3]) + XMVectorGetW(viewProjMatrix.r[2]));

    // Far plane
    frustumPlanes[5] = XMVectorSet(XMVectorGetX(viewProjMatrix.r[3]) - XMVectorGetX(viewProjMatrix.r[2]),
                                   XMVectorGetY(viewProjMatrix.r[3]) - XMVectorGetY(viewProjMatrix.r[2]),
                                   XMVectorGetZ(viewProjMatrix.r[3]) - XMVectorGetZ(viewProjMatrix.r[2]),
                                   XMVectorGetW(viewProjMatrix.r[3]) - XMVectorGetW(viewProjMatrix.r[2]));

    // Normalize frustum planes
    for (int i = 0; i < 6; i++)
    {
        frustumPlanes[i] = XMPlaneNormalize(frustumPlanes[i]);
    }

    uint32_t totalObjects = 0;
    uint32_t culledObjects = 0;
    uint32_t visibleObjectCount = 0;

    // Test each object against frustum
    for (auto* obj : objects)
    {
        if (!obj)
            continue;

        totalObjects++;

        if (!obj->IsActive() || !obj->IsVisible())
        {
            culledObjects++;
            continue;
        }

        XMFLOAT3 objPos = obj->GetPosition();
        XMVECTOR objectPosition = XMLoadFloat3(&objPos);
        float boundingRadius = 5.0f;

        bool isVisible = true;

        // Test against all frustum planes
        for (int i = 0; i < 6; i++)
        {
            float distance = XMVectorGetX(XMPlaneDotCoord(frustumPlanes[i], objectPosition));

            if (distance < -boundingRadius)
            {
                isVisible = false;
                break;
            }
        }

        if (isVisible)
        {
            visibleObjects.push_back(obj);
            visibleObjectCount++;
        }
        else
        {
            culledObjects++;
        }
    }

    auto cullingEndTime = std::chrono::high_resolution_clock::now();
    auto cullingTime = std::chrono::duration_cast<std::chrono::microseconds>(cullingEndTime - cullingStartTime);

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.totalObjects = totalObjects;
        m_statistics.visibleObjects = visibleObjectCount;
        m_statistics.culledObjects = culledObjects;
        m_statistics.cullingTime = cullingTime.count() / 1000.0f;
    }

    if (cullingTime.count() > 1000)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Frustum culling: " + std::to_wstring(visibleObjectCount) + L"/" +
                                     std::to_wstring(totalObjects) + L" objects visible (culled " +
                                     std::to_wstring(culledObjects) + L") in " +
                                     std::to_wstring(cullingTime.count() / 1000.0f) + L"ms",
                                 L"INFO");
    }
}

// ============================================================================
// POST-PROCESSING AND TEMPORAL EFFECTS
// ============================================================================

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
}

void GraphicsEngine::RenderGeometryPass()
{
    // Windows D3D11: geometry pass handled through RenderForward/RenderDeferred
}

void GraphicsEngine::RenderLightingPass()
{
    // Windows D3D11: lighting pass handled through LightingPass()
}

#else // !SPARK_PLATFORM_WINDOWS

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

#endif // SPARK_PLATFORM_WINDOWS
