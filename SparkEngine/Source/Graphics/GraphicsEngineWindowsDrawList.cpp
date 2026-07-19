/**
 * @file GraphicsEngineWindowsDrawList.cpp
 * @brief Windows/D3D11 ECS mesh draw-list processing for GraphicsEngine
 *
 * ProcessDrawList split out of GraphicsEngineWindows.cpp (which keeps
 * lifecycle: construction, device-attach initialization, shutdown, and
 * resize). The cross-platform SubmitMeshForRendering producer lives in the
 * shared GraphicsEngineSubmit.cpp, which also holds the non-Windows
 * ProcessDrawList.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"

#include "AssetPipeline.h"
#include "GPUDrivenRenderer.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
#include <wrl.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Centralized logging macros
#include "../Utils/LogMacros.h"

// ============================================================================
// ECS MESH DRAW SUBMISSION
// ============================================================================
// NOTE: SubmitMeshForRendering lives in the shared GraphicsEngineSubmit.cpp
// so Linux/macOS builds can drive the draw list too. ProcessDrawList below
// stays Windows-only — it touches D3D11 constant buffers, the GPU-driven
// renderer, and the asset pipeline's D3D11 loaders.

void GraphicsEngine::ProcessDrawList(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix)
{
    // Swap the draw list with a persistent processing buffer under the spinlock —
    // minimal hold time. std::swap preserves the allocated capacity on both
    // vectors, so after the first frame's capacity is established neither side
    // allocates or frees heap memory on the steady-state submission path.
    {
        SpinlockGuard guard(m_drawListSpinlock);
        std::swap(m_drawList, m_processingDrawList);
    }

    // Alias for readability; m_processingDrawList now owns this frame's commands.
    std::vector<MeshDrawCommand>& localDrawList = m_processingDrawList;

    if (localDrawList.empty())
        return;

    // GPU-driven culling: group draws by mesh and use indirect draw per batch.
    // When enabled, instances sharing the same mesh are culled on the GPU via
    // frustum + HiZ compute shaders, then drawn with DrawIndexedInstancedIndirect.
    // Unique-mesh draws that can't batch fall through to the CPU path below.
    if (m_settings.gpuDrivenRendering && m_assetPipeline)
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.IsInitialized())
        {
            // Sort by mesh path to batch instances of the same mesh
            std::sort(localDrawList.begin(), localDrawList.end(),
                      [](const MeshDrawCommand& a, const MeshDrawCommand& b) { return a.meshPath < b.meshPath; });

            SetBasicShaders();
            size_t i = 0;
            while (i < localDrawList.size())
            {
                // Find the range of commands sharing the same mesh
                size_t batchStart = i;
                std::string_view batchMesh = localDrawList[i].meshPath;
                while (i < localDrawList.size() && localDrawList[i].meshPath == batchMesh)
                    ++i;
                uint32_t batchCount = static_cast<uint32_t>(i - batchStart);

                // Look up the mesh asset for vertex/index buffers and AABB.
                // LoadMesh is cached — the std::string argument is required by the
                // current loader signature but the hot lookup path inside is
                // transparent-hash and allocation-free.
                auto meshAsset = m_assetPipeline->LoadMesh(std::string(batchMesh));
                if (!meshAsset || !meshAsset->GetVertexBuffer() || !meshAsset->GetIndexBuffer())
                    continue;

                const auto& meshData = meshAsset->GetMeshData();
                XMFLOAT3 bbMin = meshData.boundingBoxMin;
                XMFLOAT3 bbMax = meshData.boundingBoxMax;

                // Build per-instance AABBs by transforming the mesh AABB
                std::vector<Spark::Graphics::GPUInstanceAABB> aabbs;
                aabbs.reserve(batchCount);
                for (size_t j = batchStart; j < batchStart + batchCount; ++j)
                {
                    XMMATRIX world = XMLoadFloat4x4(&localDrawList[j].worldMatrix);

                    // Conservative AABB transform: project all 8 corners
                    XMFLOAT3 corners[8] = {
                        {bbMin.x, bbMin.y, bbMin.z}, {bbMax.x, bbMin.y, bbMin.z}, {bbMin.x, bbMax.y, bbMin.z},
                        {bbMax.x, bbMax.y, bbMin.z}, {bbMin.x, bbMin.y, bbMax.z}, {bbMax.x, bbMin.y, bbMax.z},
                        {bbMin.x, bbMax.y, bbMax.z}, {bbMax.x, bbMax.y, bbMax.z},
                    };

                    Spark::Graphics::GPUInstanceAABB aabb;
                    aabb.minX = aabb.minY = aabb.minZ = FLT_MAX;
                    aabb.maxX = aabb.maxY = aabb.maxZ = -FLT_MAX;
                    for (const auto& c : corners)
                    {
                        XMVECTOR pt = XMVector3Transform(XMLoadFloat3(&c), world);
                        XMFLOAT3 tp;
                        XMStoreFloat3(&tp, pt);
                        aabb.minX = std::min(aabb.minX, tp.x);
                        aabb.minY = std::min(aabb.minY, tp.y);
                        aabb.minZ = std::min(aabb.minZ, tp.z);
                        aabb.maxX = std::max(aabb.maxX, tp.x);
                        aabb.maxY = std::max(aabb.maxY, tp.y);
                        aabb.maxZ = std::max(aabb.maxZ, tp.z);
                    }
                    aabbs.push_back(aabb);
                }

                // Bind material from first instance (batch shares mesh, material may vary).
                // string_view overload avoids per-draw-call std::string allocation.
                m_assetPipeline->BindMaterial(localDrawList[batchStart].materialPath);

                // GPU cull + indirect draw
                ID3D11Buffer* vb = meshAsset->GetVertexBuffer();
                ID3D11Buffer* ib = meshAsset->GetIndexBuffer();
                uint32_t vertexStride = static_cast<uint32_t>(sizeof(MeshAssetData::Vertex));
                gpuRenderer.CullAndDraw(aabbs.data(), batchCount, viewMatrix, projMatrix, ib, vb, vertexStride,
                                        meshAsset->GetIndexCount());

                m_statistics.drawCalls += gpuRenderer.GetVisibleCount();
            }
            localDrawList.clear();
            return;
        }
    }

    // CPU draw path: sort by material to minimize state changes
    std::sort(localDrawList.begin(), localDrawList.end(),
              [](const MeshDrawCommand& a, const MeshDrawCommand& b) { return a.materialPath < b.materialPath; });

    // Set up shaders for ECS mesh rendering
    SetBasicShaders();

    std::string_view lastMaterial;
    for (const auto& cmd : localDrawList)
    {
        XMMATRIX world = XMLoadFloat4x4(&cmd.worldMatrix);

        // Update per-object constant buffer with world/view/proj matrices
        UpdateBasicConstants(world, viewMatrix, projMatrix);

        // Bind mesh and material through the asset pipeline, then draw.
        // string_view overloads + transparent-hash lookup keep this allocation-free
        // on the per-draw-call inner loop.
        if (m_assetPipeline)
        {
            m_assetPipeline->BindMesh(cmd.meshPath);
            // Only rebind material when it changes (sorted order)
            if (cmd.materialPath != lastMaterial)
            {
                m_assetPipeline->BindMaterial(cmd.materialPath);
                lastMaterial = cmd.materialPath;
            }
            m_assetPipeline->DrawBoundMesh();
        }

        m_statistics.drawCalls++;
    }

    // Clear the processing buffer once drained so the next frame's swap
    // delivers an empty (but capacity-preserving) buffer back to m_drawList.
    localDrawList.clear();
}

#endif // SPARK_PLATFORM_WINDOWS
