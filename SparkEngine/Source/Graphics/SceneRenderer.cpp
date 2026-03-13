#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "SceneRenderer.h"
#include "../Utils/Validate.h"
#include <algorithm>

namespace Spark::Graphics
{

    const std::string SceneRenderer::s_emptyString;

    bool SceneRenderer::Initialize(uint32_t maxDrawCommands)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, maxDrawCommands > 0, false);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SceneRenderer initializing with maxDrawCommands=%u",
                       maxDrawCommands);
        m_maxDrawCommands = maxDrawCommands;
        m_drawCommands.reserve(maxDrawCommands);
        m_visibleCommands.reserve(maxDrawCommands);
        return true;
    }

    void SceneRenderer::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SceneRenderer shutting down (%zu draw commands in flight)",
                       m_drawCommands.size());
        m_drawCommands.clear();
        m_visibleCommands.clear();
        m_pathLookup.clear();
    }

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

    void SceneRenderer::CullAndSort(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                    const DirectX::XMFLOAT3& cameraPos)
    {
        m_visibleCommands.clear();

        // Build view-projection matrix for frustum extraction
        DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(viewMatrix, projMatrix);
        DirectX::XMVECTOR cameraPosVec = DirectX::XMLoadFloat3(&cameraPos);

        // Frustum culling: test each command's world position against the frustum
        for (auto& cmd : m_drawCommands)
        {
            // Extract position from world matrix
            DirectX::XMFLOAT3 objPos(cmd.worldMatrix._41, cmd.worldMatrix._42, cmd.worldMatrix._43);
            DirectX::XMVECTOR objPosVec = DirectX::XMLoadFloat3(&objPos);

            // Compute distance to camera for sorting
            DirectX::XMVECTOR diff = DirectX::XMVectorSubtract(objPosVec, cameraPosVec);
            DirectX::XMVECTOR distSq = DirectX::XMVector3LengthSq(diff);
            DirectX::XMStoreFloat(&cmd.distanceToCamera, distSq);

            // Simple frustum check: transform point to clip space and check bounds
            DirectX::XMVECTOR clipPos =
                DirectX::XMVector4Transform(DirectX::XMVectorSet(objPos.x, objPos.y, objPos.z, 1.0f), viewProj);

            float w = DirectX::XMVectorGetW(clipPos);
            if (w > 0.0f)
            {
                float x = DirectX::XMVectorGetX(clipPos) / w;
                float y = DirectX::XMVectorGetY(clipPos) / w;
                float z = DirectX::XMVectorGetZ(clipPos) / w;

                // Conservative frustum bounds (allow some margin for object size)
                constexpr float kMargin = 2.0f;
                if (x >= -kMargin && x <= kMargin && y >= -kMargin && y <= kMargin && z >= -0.1f && z <= 1.1f)
                {
                    m_visibleCommands.push_back(cmd);
                }
            }
        }

        // Sort by sort key (material batching), then by distance (front-to-back for opaque)
        std::sort(m_visibleCommands.begin(), m_visibleCommands.end(),
                  [](const DrawCommand& a, const DrawCommand& b)
                  {
                      if (a.sortKey != b.sortKey)
                          return a.sortKey < b.sortKey;
                      return a.distanceToCamera < b.distanceToCamera;
                  });
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
