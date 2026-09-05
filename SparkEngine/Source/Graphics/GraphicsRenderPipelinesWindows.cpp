/**
 * @file GraphicsRenderPipelinesWindows.cpp
 * @brief D3D11 rendering pipeline implementations (Forward, Deferred, Forward+)
 *
 * Contains the pipeline entry points for the D3D11 backend. G-Buffer fill,
 * lighting pass, frustum culling, post-processing, and temporal effects live
 * in GraphicsRenderPipelinesWindowsPasses.cpp.
 * Linux counterpart lives in GraphicsRenderPipelinesLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "LightingSystem.h"
#include "Mesh.h"
#include "TerrainRenderer.h"
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
#include "Core/Platform.h"
#include <wrl.h>

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
                if (const Mesh* mesh = obj->GetMesh())
                {
                    triangles += mesh->GetIndexCount() / 3;
                    vertices += mesh->GetVertexCount();
                }
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

    // Phase 1: Fill G-Buffer
    FillGBuffer(objects, viewMatrix, projMatrix);

    // Phase 2: Lighting pass
    LightingPass(viewMatrix, projMatrix);

    // Phase 3: Forward rendering for transparent objects
    uint32_t transparentDrawCalls = 0;
    uint32_t transparentTriangles = 0;
    uint32_t transparentVertices = 0;
    for (auto* obj : objects)
    {
        if (obj && obj->IsActive() && obj->IsVisible())
        {
            try
            {
                obj->Render(viewMatrix, projMatrix);
                transparentDrawCalls++;
                if (const Mesh* mesh = obj->GetMesh())
                {
                    transparentTriangles += mesh->GetIndexCount() / 3;
                    transparentVertices += mesh->GetVertexCount();
                }
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
        m_statistics.triangles += transparentTriangles;
        m_statistics.vertices += transparentVertices;
    }
}

void GraphicsEngine::RenderForwardPlus(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                       const std::vector<GameObject*>& objects)
{
    if (!m_context)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "RenderForwardPlus: device context is null, skipping");
        return;
    }

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
                if (const Mesh* mesh = obj->GetMesh())
                {
                    triangles += mesh->GetIndexCount() / 3;
                    vertices += mesh->GetVertexCount();
                }
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
}


#endif // SPARK_PLATFORM_WINDOWS
