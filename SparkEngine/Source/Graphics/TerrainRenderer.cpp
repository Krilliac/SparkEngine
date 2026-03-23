/**
 * @file TerrainRenderer.cpp
 * @brief Engine-level terrain renderer — heightmap mesh generation and GPU rendering
 *
 * Builds triangle meshes from TerrainComponent heightmap data and manages
 * GPU vertex/index buffers. Terrain meshes are rebuilt only when marked dirty.
 */

#include "TerrainRenderer.h"
#include "../Utils/ContainerUtils.h"
#include "../Utils/Validate.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace Spark::Graphics
{

#ifdef SPARK_PLATFORM_WINDOWS

    bool TerrainRenderer::Initialize(ID3D11Device* device)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Graphics, device != nullptr, false);
        m_device = device;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TerrainRenderer initialized");
        return true;
    }

#else

    bool TerrainRenderer::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TerrainRenderer initialized (CPU-only mode)");
        return true;
    }

#endif

    void TerrainRenderer::Shutdown()
    {
        m_terrainData.clear();
#ifdef SPARK_PLATFORM_WINDOWS
        m_device = nullptr;
#endif
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "TerrainRenderer shut down");
    }

    void TerrainRenderer::UpdateTerrains(const std::unordered_map<uint32_t, const TerrainComponent*>& terrains)
    {
        // Remove stale entries (entities no longer have terrain)
        std::vector<uint32_t> toRemove;
        for (const auto& [id, data] : m_terrainData)
        {
            if (!Spark::ContainerUtils::Contains(terrains, id))
                toRemove.push_back(id);
        }
        for (uint32_t id : toRemove)
            m_terrainData.erase(id);

        // Update or create entries for active terrains
        for (const auto& [entityID, component] : terrains)
        {
            if (!component || !component->visible)
                continue;

            auto it = m_terrainData.find(entityID);
            if (it == m_terrainData.end())
            {
                // New terrain — create entry and mark dirty
                TerrainGPUData data;
                data.entityID = entityID;
                data.meshDirty = true;
                m_terrainData[entityID] = std::move(data);
                it = m_terrainData.find(entityID);
            }

            if (it->second.meshDirty)
            {
                std::vector<TerrainVertex> vertices;
                std::vector<uint32_t> indices;
                BuildMeshFromComponent(*component, vertices, indices);

                it->second.vertexCount = static_cast<uint32_t>(vertices.size());
                it->second.indexCount = static_cast<uint32_t>(indices.size());

#ifdef SPARK_PLATFORM_WINDOWS
                if (m_device && !vertices.empty())
                {
                    CreateGPUBuffers(it->second, vertices, indices);
                }
#endif

                it->second.meshDirty = false;
            }
        }
    }

    void TerrainRenderer::MarkDirty(uint32_t entityID)
    {
        auto it = m_terrainData.find(entityID);
        if (it != m_terrainData.end())
            it->second.meshDirty = true;
    }

    void TerrainRenderer::RemoveTerrain(uint32_t entityID)
    {
        m_terrainData.erase(entityID);
    }

    void TerrainRenderer::BuildMeshFromComponent(const TerrainComponent& component,
                                                 std::vector<TerrainVertex>& outVertices,
                                                 std::vector<uint32_t>& outIndices)
    {
        const int resolution = component.heightmapResolution;
        if (resolution < 2 || component.heightmap.empty())
            return;

        // Determine actual grid size from available data
        const int gridSize = std::min(resolution, static_cast<int>(std::sqrt(component.heightmap.size())));
        if (gridSize < 2)
            return;

        const float cellSize = component.terrainSize / static_cast<float>(gridSize - 1);
        const float halfSize = component.terrainSize * 0.5f;

        // Generate vertices
        outVertices.reserve(static_cast<size_t>(gridSize) * gridSize);
        for (int z = 0; z < gridSize; ++z)
        {
            for (int x = 0; x < gridSize; ++x)
            {
                size_t idx = static_cast<size_t>(z) * gridSize + x;
                float height = (idx < component.heightmap.size()) ? component.heightmap[idx] : 0.0f;
                height = std::clamp(height, component.minHeight, component.maxHeight) * component.heightScale;

                TerrainVertex vert;
                vert.position = {x * cellSize - halfSize, height, z * cellSize - halfSize};

                // UV coordinates
                vert.texCoord = {static_cast<float>(x) / (gridSize - 1), static_cast<float>(z) / (gridSize - 1)};

                // Splatmap weights (default to layer 0 if no splatmap)
                vert.splatWeights = {1.0f, 0.0f, 0.0f, 0.0f};
                if (!component.splatmap.empty() && component.splatmapResolution > 0)
                {
                    // Map grid position to splatmap position
                    float su = static_cast<float>(x) / (gridSize - 1) * (component.splatmapResolution - 1);
                    float sv = static_cast<float>(z) / (gridSize - 1) * (component.splatmapResolution - 1);
                    int si = std::clamp(static_cast<int>(su), 0, component.splatmapResolution - 1);
                    int sj = std::clamp(static_cast<int>(sv), 0, component.splatmapResolution - 1);
                    size_t splatIdx = (static_cast<size_t>(sj) * component.splatmapResolution + si) * 4;
                    if (splatIdx + 3 < component.splatmap.size())
                    {
                        vert.splatWeights.x = component.splatmap[splatIdx] / 255.0f;
                        vert.splatWeights.y = component.splatmap[splatIdx + 1] / 255.0f;
                        vert.splatWeights.z = component.splatmap[splatIdx + 2] / 255.0f;
                        vert.splatWeights.w = component.splatmap[splatIdx + 3] / 255.0f;
                    }
                }

                // Normal computed below
                vert.normal = {0.0f, 1.0f, 0.0f};
                outVertices.push_back(vert);
            }
        }

        // Compute normals from height gradients (central differences)
        for (int z = 0; z < gridSize; ++z)
        {
            for (int x = 0; x < gridSize; ++x)
            {
                float hL = (x > 0) ? outVertices[static_cast<size_t>(z) * gridSize + x - 1].position.y : 0.0f;
                float hR =
                    (x < gridSize - 1) ? outVertices[static_cast<size_t>(z) * gridSize + x + 1].position.y : 0.0f;
                float hD = (z > 0) ? outVertices[static_cast<size_t>((z - 1)) * gridSize + x].position.y : 0.0f;
                float hU =
                    (z < gridSize - 1) ? outVertices[static_cast<size_t>((z + 1)) * gridSize + x].position.y : 0.0f;

                XMFLOAT3 normal = {hL - hR, 2.0f * cellSize, hD - hU};
                XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&normal));
                XMStoreFloat3(&outVertices[static_cast<size_t>(z) * gridSize + x].normal, n);
            }
        }

        // Generate triangle indices
        outIndices.reserve(static_cast<size_t>((gridSize - 1)) * (gridSize - 1) * 6);
        for (int z = 0; z < gridSize - 1; ++z)
        {
            for (int x = 0; x < gridSize - 1; ++x)
            {
                uint32_t topLeft = static_cast<uint32_t>(z * gridSize + x);
                uint32_t topRight = topLeft + 1;
                uint32_t bottomLeft = static_cast<uint32_t>((z + 1) * gridSize + x);
                uint32_t bottomRight = bottomLeft + 1;

                // Triangle 1
                outIndices.push_back(topLeft);
                outIndices.push_back(bottomLeft);
                outIndices.push_back(topRight);

                // Triangle 2
                outIndices.push_back(topRight);
                outIndices.push_back(bottomLeft);
                outIndices.push_back(bottomRight);
            }
        }
    }

#ifdef SPARK_PLATFORM_WINDOWS

    bool TerrainRenderer::CreateGPUBuffers(TerrainGPUData& data, const std::vector<TerrainVertex>& vertices,
                                           const std::vector<uint32_t>& indices)
    {
        if (!m_device || vertices.empty() || indices.empty())
            return false;

        // Create vertex buffer
        D3D11_BUFFER_DESC vbDesc{};
        vbDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(TerrainVertex));
        vbDesc.Usage = D3D11_USAGE_DEFAULT;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData{};
        vbData.pSysMem = vertices.data();

        HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, data.vertexBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create terrain vertex buffer (HRESULT: 0x%08X)",
                            hr);
            return false;
        }

        // Create index buffer
        D3D11_BUFFER_DESC ibDesc{};
        ibDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        ibDesc.Usage = D3D11_USAGE_DEFAULT;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ibData{};
        ibData.pSysMem = indices.data();

        hr = m_device->CreateBuffer(&ibDesc, &ibData, data.indexBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create terrain index buffer (HRESULT: 0x%08X)",
                            hr);
            return false;
        }

        return true;
    }

    void TerrainRenderer::Render(ID3D11DeviceContext* context, const DirectX::XMMATRIX& /*viewMatrix*/,
                                 const DirectX::XMMATRIX& /*projMatrix*/)
    {
        if (!context)
            return;

        for (const auto& [id, data] : m_terrainData)
        {
            if (!data.vertexBuffer || !data.indexBuffer || data.indexCount == 0)
                continue;

            // Bind vertex buffer
            UINT stride = sizeof(TerrainVertex);
            UINT offset = 0;
            ID3D11Buffer* vb = data.vertexBuffer.Get();
            context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

            // Bind index buffer
            context->IASetIndexBuffer(data.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

            // Set triangle list topology
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // Issue draw call
            context->DrawIndexed(data.indexCount, 0, 0);
        }
    }

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
