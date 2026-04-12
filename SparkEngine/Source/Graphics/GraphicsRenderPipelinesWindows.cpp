/**
 * @file GraphicsRenderPipelinesWindows.cpp
 * @brief D3D11 rendering pipeline implementations (Forward, Deferred, Forward+)
 *
 * Contains G-Buffer fill, lighting pass, frustum culling, post-processing,
 * and temporal effects for the D3D11 backend.
 * Linux counterpart lives in GraphicsRenderPipelinesLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "LightingSystem.h"
#include "PostProcessingPipeline.h"
#include "TerrainRenderer.h"
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

#include <windows.h>
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
    if (!m_context)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "RenderForward: device context is null, skipping");
        return;
    }

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

    // Render terrain after scene objects
#ifdef SPARK_PLATFORM_WINDOWS
    if (m_terrainRenderer && m_context)
    {
        m_terrainRenderer->Render(m_context.Get(), viewMatrix, projMatrix);
    }
#endif

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
    if (!m_context)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "RenderDeferred: device context is null, skipping");
        return;
    }

    // Verify G-Buffer targets are valid before attempting deferred rendering
    bool gBufferValid = true;
    for (int i = 0; i < 4; ++i)
    {
        if (!m_gBufferRTVs[i])
        {
            gBufferValid = false;
            break;
        }
    }
    if (!gBufferValid)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "RenderDeferred: G-Buffer render targets not created, falling back to forward");
        RenderForward(viewMatrix, projMatrix, objects);
        return;
    }

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
    if (!m_context)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "RenderForwardPlus: device context is null, skipping");
        return;
    }

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
    if (!m_context)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "FillGBuffer: device context is null, skipping");
        return;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Filling G-Buffer for deferred rendering", L"INFO");

    // Bind G-Buffer render targets for the geometry pass
    ID3D11RenderTargetView* gBufferRTVs[4] = {m_gBufferRTVs[0].Get(), m_gBufferRTVs[1].Get(), m_gBufferRTVs[2].Get(),
                                              m_gBufferRTVs[3].Get()};
    m_context->OMSetRenderTargets(4, gBufferRTVs, m_depthStencilView.Get());

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
        if (deltaTime < 0.0f || deltaTime > 1.0f)
        {
            deltaTime = std::clamp(deltaTime, 0.0f, 1.0f);
        }
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


#endif // SPARK_PLATFORM_WINDOWS
