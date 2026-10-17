/**
 * @file GraphicsRenderPipelinesWindowsPasses.cpp
 * @brief D3D11 G-Buffer fill, lighting pass, frustum culling, and
 * post-processing passes
 *
 * Split from GraphicsRenderPipelinesWindows.cpp. Contains the deferred-pass
 * internals (G-Buffer fill, lighting pass), frustum culling, post-processing,
 * and temporal effects for the D3D11 backend.
 * Linux counterpart lives in GraphicsRenderPipelinesLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "../Game/GameObject.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"
#include "D3D11FrustumCulling.h"
#include "GraphicsEngine.h"
#include "GraphicsRenderPipelinesShadowPass.h"
#include "LightingSystem.h"
#include "Mesh.h"
#include "PostProcessingPipeline.h"
#include "TemporalEffects.h"

#include "Core/Platform.h"
#include <d3d11_1.h>
#include <windows.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

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

    ComPtr<ID3D11PixelShader> deferredShader;
    UINT shaderBytes = sizeof(ID3D11PixelShader*);
    if (!m_basicPixelShader ||
        FAILED(m_basicPixelShader->GetPrivateData(kDeferredGBufferShaderGuid, &shaderBytes,
                                                  deferredShader.GetAddressOf())) ||
        !deferredShader)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "FillGBuffer: deferred shader unavailable, skipping geometry");
        return;
    }

    // Render-thread-only borrowed binding seam, no allocation. GameObject::Render
    // calls SetBasicShaders, so replace that seam only for this call. The forward
    // shader owns the deferred variant; GetPrivateData adds the local reference.
    // Restore even on exceptions and when a caller inspects geometry without
    // subsequently calling LightingPass (the Primary geometry capture does so).
    struct ScopedGeometryShader
    {
        ComPtr<ID3D11PixelShader>& current;
        ComPtr<ID3D11PixelShader> saved;
        ID3D11DeviceContext* context;
        ScopedGeometryShader(ComPtr<ID3D11PixelShader>& basic, ComPtr<ID3D11PixelShader> next,
                             ID3D11DeviceContext* deviceContext)
            : current(basic), saved(std::move(next)), context(deviceContext)
        {
            current.Swap(saved);
        }
        ~ScopedGeometryShader()
        {
            current.Swap(saved);
            context->PSSetShader(current.Get(), nullptr, 0);
        }
        ScopedGeometryShader(const ScopedGeometryShader&) = delete;
        ScopedGeometryShader& operator=(const ScopedGeometryShader&) = delete;
    } geometryShader(m_basicPixelShader, std::move(deferredShader), m_context.Get());

    // Only the three implemented attachments are written. Motion history is
    // not available yet; do not fabricate a fourth attachment's output.
    ID3D11RenderTargetView* gBufferRTVs[3] = {m_gBufferRTVs[0].Get(), m_gBufferRTVs[1].Get(), m_gBufferRTVs[2].Get()};
    m_context->OMSetRenderTargets(3, gBufferRTVs, m_depthStencilView.Get());

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
                if (const Mesh* mesh = obj->GetMesh())
                {
                    totalTriangles += mesh->GetIndexCount() / 3;
                    totalVertices += mesh->GetVertexCount();
                }
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

    SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "G-Buffer fill complete with %u draw calls", gBufferDrawCalls);
}

void GraphicsEngine::LightingPass(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    if (!m_context)
        return;
    auto lightingStartTime = std::chrono::high_resolution_clock::now();

    if (m_context && m_renderTargetView)
    {
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
    }

    if (m_lightingSystem)
    {
        try
        {
            // Resolve packs current light values after Update; the legacy b1/b2
            // upload is not a compatible deferred constant layout.
            if (m_currentPipeline != RenderingPipeline::Deferred)
                m_lightingSystem->BindLightingData(m_context.Get());

            // Update lighting system with the real frame delta (RenderPostProcessing
            // does the same); a hard-coded 16 ms desynchronises every time-based
            // lighting term from the actual frame rate. m_statistics is written under
            // m_metricsMutex (see the drawCalls update below), so snapshot the value
            // under the same lock rather than racing the writer.
            float frameTimeMs = 0.0f;
            {
                std::lock_guard<std::mutex> lock(m_metricsMutex);
                frameTimeMs = m_statistics.frameTime;
            }
            const float deltaTime = std::clamp(frameTimeMs / 1000.0f, 0.0f, 1.0f);
            m_lightingSystem->Update(deltaTime, viewMatrix, projMatrix);

            // Render shadow casters into each light's depth target. Under the
            // render-graph pipeline this work belongs to (and runs in) the
            // ShadowPass, which executes before the geometry pass drains the
            // draw list — running it again here would draw an empty list.
            if (m_settings.shadows && m_currentPipeline != RenderingPipeline::RenderGraphBased)
            {
                try
                {
                    const std::vector<MeshDrawCommand> shadowDrawList = GetDrawList();
                    uint32_t shadowDrawCalls = 0;
                    m_lightingSystem->RenderShadowMaps(
                        [this, &shadowDrawList, &shadowDrawCalls](const XMMATRIX& lightView, const XMMATRIX& lightProj)
                        {
                            shadowDrawCalls +=
                                Spark::Graphics::RenderShadowCasterDepth(*this, shadowDrawList, lightView, lightProj);
                        });

                    std::lock_guard<std::mutex> lock(m_metricsMutex);
                    m_statistics.drawCalls += shadowDrawCalls;
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

    // Count only the fullscreen draw actually submitted; unavailable resources
    // or a failed constant-buffer upload must not fabricate a lighting draw.
    uint32_t resolvedLights = 0;
    const uint32_t lightingDrawCalls = ResolveDeferredLighting(viewMatrix, projMatrix, resolvedLights) ? 1u : 0u;
    if (m_currentPipeline == RenderingPipeline::Deferred && lightingDrawCalls == 0)
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Deferred lighting resolve skipped; no lighting draw submitted");

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
                m_statistics.activeLights =
                    m_currentPipeline == RenderingPipeline::Deferred ? resolvedLights : lightingMetrics.activeLights;
                m_statistics.shadowUpdates = lightingMetrics.shadowMapUpdates;
                m_statistics.lightCullingTime = lightingMetrics.lightCullingTime;
            }
            catch (const std::exception& e)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Could not retrieve lighting metrics: " +
                                             std::wstring(e.what(), e.what() + strlen(e.what())),
                                         L"WARNING");
                // Metrics unavailable — report nothing rather than a plausible
                // constant: a fabricated count cannot detect a regression.
                m_statistics.activeLights = 0;
                m_statistics.shadowUpdates = 0;
                m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
            }
            catch (...)
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Could not retrieve lighting metrics - unknown error", L"WARNING");
                m_statistics.activeLights = 0;
                m_statistics.shadowUpdates = 0;
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

    SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "Deferred lighting data update complete; resolve pending (%.3f ms)",
                    static_cast<double>(lightingTime.count()) / 1000.0);
}

void GraphicsEngine::CullObjects(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix,
                                 const XMMATRIX& projMatrix, std::vector<GameObject*>& visibleObjects)
{
    auto cullingStartTime = std::chrono::high_resolution_clock::now();

    visibleObjects.clear();
    visibleObjects.reserve(objects.size());

    const XMMATRIX viewProjMatrix = XMMatrixMultiply(viewMatrix, projMatrix);
    const auto frustumPlanes = Spark::Graphics::D3D11RenderMath::ExtractFrustumPlanes(viewProjMatrix);

    uint32_t totalObjects = 0;
    uint32_t culledObjects = 0;
    uint32_t visibleObjectCount = 0;

    // Test each object against frustum
    for (auto* obj : objects)
    {
        if (!obj)
        {
            continue;
        }

        totalObjects++;

        if (!obj->IsActive() || !obj->IsVisible())
        {
            culledObjects++;
            continue;
        }

        const XMFLOAT3 objPos = obj->GetPosition();
        const XMFLOAT3 scale = obj->GetScale();
        // Mesh keeps CPU vertices private and exposes no local bounds; GameObject
        // exposes no bounds either. Until that API exists, retain the legacy
        // five-unit envelope and expand it for scale (never shrink it). This is
        // conservative for the existing unit primitives, not certified arbitrary
        // imported geometry. Computing mesh bounds by GPU readback here is not
        // viable.
        const float scaleFactor = std::max({1.0f, std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
        const float boundingRadius = 5.0f * scaleFactor;
        const bool isVisible =
            Spark::Graphics::D3D11RenderMath::SphereIntersects(frustumPlanes, objPos, boundingRadius);

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
        m_postProcessing->SetInputSRV(m_backBufferSRV.Get());
        m_postProcessing->SetDepthSRV(m_depthStencilSRV.Get());
        m_postProcessing->SetOutputRTV(m_renderTargetView.Get());
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

#endif // SPARK_PLATFORM_WINDOWS
