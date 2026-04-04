#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file SceneRenderer.cpp
 * @brief High-level scene rendering: draw command submission, frustum culling, and sorting
 *
 * Collects draw commands each frame via Submit(), then CullAndSort() performs
 * clip-space frustum culling with a conservative margin and sorts visible
 * commands by material (batching) then by distance (front-to-back for opaque).
 * Uses a per-frame linear allocator to minimize allocation overhead.
 */

#include "SceneRenderer.h"
#include "DrawSortKey.h"
#include "../Core/EngineSettings.h"
#include "../Utils/Validate.h"
#include <algorithm>

namespace Spark::Graphics
{

    const std::string SceneRenderer::s_emptyString;

    /// Pre-allocates draw command storage up to the specified maximum.
    bool SceneRenderer::Initialize(uint32_t maxDrawCommands)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, maxDrawCommands > 0, false);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SceneRenderer initializing with maxDrawCommands=%u",
                       maxDrawCommands);
        m_maxDrawCommands = maxDrawCommands;
        m_drawCommands.reserve(maxDrawCommands);
        m_visibleCommands.reserve(maxDrawCommands);
        m_portalCulling.Initialize();
        return true;
    }

    void SceneRenderer::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SceneRenderer shutting down (%zu draw commands in flight)",
                       m_drawCommands.size());
        m_portalCulling.Shutdown();
        m_drawCommands.clear();
        m_visibleCommands.clear();
        m_pathLookup.clear();
    }

    /// Clears all draw commands and resets the per-frame linear allocator for the new frame.
    void SceneRenderer::BeginFrame()
    {
        m_drawCommands.clear();
        m_visibleCommands.clear();
        m_frameAllocator.UpdatePeak();
        m_frameAllocator.Reset();
    }

    void SceneRenderer::Submit(AssetHandle meshHandle, AssetHandle materialHandle, const DirectX::XMMATRIX& worldMatrix,
                               bool castShadows)
    {
        DrawCommand cmd;
        cmd.mesh = meshHandle;
        cmd.material = materialHandle;
        DirectX::XMStoreFloat4x4(&cmd.worldMatrix, worldMatrix);
        cmd.castShadows = castShadows;
        m_drawCommands.push_back(cmd);
    }

    void SceneRenderer::Submit(const std::string& meshPath, const std::string& materialPath,
                               const DirectX::XMMATRIX& worldMatrix, bool castShadows)
    {
        AssetHandle meshHandle = AssetHandle::FromString(meshPath);
        AssetHandle materialHandle = AssetHandle::FromString(materialPath);

        // Register path lookup for asset resolution
        m_pathLookup.emplace(meshHandle, meshPath);
        m_pathLookup.emplace(materialHandle, materialPath);

        Submit(meshHandle, materialHandle, worldMatrix, castShadows);
    }

    /// Frustum-culls draw commands and sorts survivors for efficient rendering.
    /// Uses DrawSortKey to build 64-bit composite sort keys encoding render layer,
    /// shader hash, material hash, and quantized depth for optimal GPU state batching.
    /// Falls back to radix sort for large draw lists (>64 entries).
    void SceneRenderer::CullAndSort(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                    const DirectX::XMFLOAT3& cameraPos)
    {
        m_visibleCommands.clear();

        DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(viewMatrix, projMatrix);
        DirectX::XMVECTOR cameraPosVec = DirectX::XMLoadFloat3(&cameraPos);

        // Portal culling: determine visible cells before frustum testing
        bool usePortalCulling =
            EngineSettings::GetInstance().Rendering().portalCulling && m_portalCulling.GetCellCount() > 0;
        if (usePortalCulling)
        {
            DirectX::XMFLOAT4X4 vpStored;
            DirectX::XMStoreFloat4x4(&vpStored, viewProj);
            m_portalCulling.DetermineVisibility(cameraPos, vpStored);
        }

        // Build sorted draw list using DrawSortKey for optimal state batching
        std::vector<DrawSortEntry> sortEntries;
        sortEntries.reserve(m_drawCommands.size());

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_drawCommands.size()); ++i)
        {
            auto& cmd = m_drawCommands[i];

            // Extract position from world matrix
            DirectX::XMFLOAT3 objPos(cmd.worldMatrix._41, cmd.worldMatrix._42, cmd.worldMatrix._43);
            DirectX::XMVECTOR objPosVec = DirectX::XMLoadFloat3(&objPos);

            // Portal culling: skip objects in non-visible cells
            if (usePortalCulling)
            {
                CellId objCell = m_portalCulling.FindCell(objPos);
                if (objCell != kInvalidCell && !m_portalCulling.IsCellVisible(objCell))
                {
                    continue;
                }
            }

            // Compute distance to camera
            DirectX::XMVECTOR diff = DirectX::XMVectorSubtract(objPosVec, cameraPosVec);
            DirectX::XMVECTOR distSq = DirectX::XMVector3LengthSq(diff);
            DirectX::XMStoreFloat(&cmd.distanceToCamera, distSq);

            // Frustum check: transform point to clip space and test bounds
            DirectX::XMVECTOR clipPos =
                DirectX::XMVector4Transform(DirectX::XMVectorSet(objPos.x, objPos.y, objPos.z, 1.0f), viewProj);

            float w = DirectX::XMVectorGetW(clipPos);
            if (w > 0.0f)
            {
                float x = DirectX::XMVectorGetX(clipPos) / w;
                float y = DirectX::XMVectorGetY(clipPos) / w;
                float z = DirectX::XMVectorGetZ(clipPos) / w;

                constexpr float kMargin = 2.0f;
                if (x >= -kMargin && x <= kMargin && y >= -kMargin && y <= kMargin && z >= -0.1f && z <= 1.1f)
                {
                    // Build 64-bit sort key from mesh/material hashes and view depth
                    float viewDepth = z * w; // Linearized view-space depth
                    uint32_t shaderHash = static_cast<uint32_t>(cmd.mesh.hash >> 32);
                    uint32_t materialHash = static_cast<uint32_t>(cmd.material.hash);

                    DrawSortEntry entry;
                    entry.key = DrawSortKey::BuildOpaque(shaderHash, materialHash, viewDepth);
                    entry.drawIndex = i;

                    // Store the sort key back in the command for downstream use
                    cmd.sortKey = static_cast<uint32_t>(entry.key >> 32);

                    sortEntries.push_back(entry);
                }
            }
        }

        // Use radix sort for large lists, std::sort for small ones
        DrawSortKey::RadixSort(sortEntries);

        // Build visible command list in sorted order
        m_visibleCommands.reserve(sortEntries.size());
        for (const auto& entry : sortEntries)
        {
            m_visibleCommands.push_back(m_drawCommands[entry.drawIndex]);
        }
    }

    void SceneRenderer::EndFrame()
    {
        m_frameAllocator.UpdatePeak();
        m_frameAllocator.Reset();
    }

    const std::string& SceneRenderer::ResolvePath(AssetHandle handle) const
    {
        auto it = m_pathLookup.find(handle);
        return (it != m_pathLookup.end()) ? it->second : s_emptyString;
    }

    void SceneRenderer::RegisterAssetPath(AssetHandle handle, const std::string& path)
    {
        m_pathLookup.emplace(handle, path);
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
