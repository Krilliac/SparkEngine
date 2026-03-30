/**
 * @file MeshShaderPipeline.cpp
 * @brief Mesh shader rendering pipeline implementation
 * @author Spark Engine Team
 * @date 2026
 */

#include "MeshShaderPipeline.h"
#include "../Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <unordered_set>

namespace Spark::Graphics
{

    bool MeshShaderPipeline::CheckMeshShaderSupport()
    {
        // Mesh shaders require D3D12 with SM 6.5 or Vulkan with VK_EXT_mesh_shader.
        // D3D11 does not support mesh shaders.
        // This check would query D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier
        // or vkGetPhysicalDeviceFeatures2 for VkPhysicalDeviceMeshShaderFeaturesEXT.
        // For now, return false (D3D11 primary backend).
        return false;
    }

    bool MeshShaderPipeline::Initialize(void* device)
    {
        m_stats.meshShadersAvailable = CheckMeshShaderSupport();

        if (!m_stats.meshShadersAvailable)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "Mesh shaders not available; falling back to traditional draw calls");
        }
        else
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Mesh shader pipeline initialized with hardware support");
        }

        m_initialized = true;
        return true;
    }

    void MeshShaderPipeline::Shutdown()
    {
        m_stats = {};
        m_initialized = false;
    }

    MeshletMesh MeshShaderPipeline::BuildMeshlets(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                                  uint32_t indexCount, uint32_t maxVerticesPerMeshlet,
                                                  uint32_t maxTrianglesPerMeshlet) const
    {
        MeshletMesh result;
        result.totalVertices = vertexCount;
        result.totalTriangles = indexCount / 3;

        if (!vertices || !indices || indexCount < 3)
            return result;

        // Greedy meshlet builder: scan triangles and pack into meshlets
        // until we hit the vertex or triangle limit per meshlet
        uint32_t triCount = indexCount / 3;
        std::vector<bool> triUsed(triCount, false);

        uint32_t globalUniqueVertexOffset = 0;
        uint32_t globalPrimitiveOffset = 0;

        for (uint32_t startTri = 0; startTri < triCount;)
        {
            GPUMeshlet meshlet = {};
            meshlet.vertexOffset = globalUniqueVertexOffset;
            meshlet.triangleOffset = globalPrimitiveOffset;

            // Track unique vertices in this meshlet
            std::unordered_set<uint32_t> meshletVertices;
            std::vector<uint32_t> meshletUniqueVerts;
            std::vector<uint32_t> meshletPrimitives;

            for (uint32_t tri = startTri; tri < triCount; tri++)
            {
                if (triUsed[tri])
                    continue;

                uint32_t i0 = indices[tri * 3 + 0];
                uint32_t i1 = indices[tri * 3 + 1];
                uint32_t i2 = indices[tri * 3 + 2];

                // Count new vertices this triangle would add
                uint32_t newVerts = 0;
                if (meshletVertices.find(i0) == meshletVertices.end())
                    newVerts++;
                if (meshletVertices.find(i1) == meshletVertices.end())
                    newVerts++;
                if (meshletVertices.find(i2) == meshletVertices.end())
                    newVerts++;

                // Check limits
                if (meshletVertices.size() + newVerts > maxVerticesPerMeshlet)
                    continue;
                if (meshletPrimitives.size() >= maxTrianglesPerMeshlet)
                    break;

                // Add vertices
                auto addVertex = [&](uint32_t idx)
                {
                    if (meshletVertices.insert(idx).second)
                    {
                        meshletUniqueVerts.push_back(idx);
                    }
                };

                addVertex(i0);
                addVertex(i1);
                addVertex(i2);

                // Map global indices to meshlet-local indices
                auto localIndex = [&](uint32_t globalIdx) -> uint8_t
                {
                    for (uint32_t j = 0; j < meshletUniqueVerts.size(); j++)
                    {
                        if (meshletUniqueVerts[j] == globalIdx)
                            return static_cast<uint8_t>(j);
                    }
                    return 0;
                };

                // Pack 3 local indices into a uint32
                uint32_t packed = static_cast<uint32_t>(localIndex(i0)) | (static_cast<uint32_t>(localIndex(i1)) << 8) |
                                  (static_cast<uint32_t>(localIndex(i2)) << 16);
                meshletPrimitives.push_back(packed);

                triUsed[tri] = true;
            }

            if (meshletUniqueVerts.empty())
            {
                // Find next unused triangle
                while (startTri < triCount && triUsed[startTri])
                    startTri++;
                continue;
            }

            meshlet.vertexCount = static_cast<uint32_t>(meshletUniqueVerts.size());
            meshlet.triangleCount = static_cast<uint32_t>(meshletPrimitives.size());

            // Compute bounding sphere
            ComputeBoundingSphere(meshlet.boundCenter, &meshlet.boundRadius, vertices, meshletUniqueVerts.data(),
                                  meshlet.vertexCount);

            // Compute normal cone for backface culling
            ComputeNormalCone(meshlet, vertices, indices);

            // Append to output
            result.meshlets.push_back(meshlet);
            result.uniqueVertexIndices.insert(result.uniqueVertexIndices.end(), meshletUniqueVerts.begin(),
                                              meshletUniqueVerts.end());
            result.primitiveIndices.insert(result.primitiveIndices.end(), meshletPrimitives.begin(),
                                           meshletPrimitives.end());

            globalUniqueVertexOffset += meshlet.vertexCount;
            globalPrimitiveOffset += meshlet.triangleCount;

            // Advance to next unused triangle
            while (startTri < triCount && triUsed[startTri])
                startTri++;
        }

        // Build a single LOD group containing all meshlets
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Built %zu meshlets from %u vertices, %u triangles",
                        result.meshlets.size(), vertexCount, indexCount / 3);

        if (!result.meshlets.empty())
        {
            GPUMeshletGroup group = {};
            group.meshletOffset = 0;
            group.meshletCount = static_cast<uint32_t>(result.meshlets.size());
            group.lodError = 0.0f;
            group.parentLodError = 0.0f;

            // Group bounding sphere = union of all meshlet bounding spheres
            float minBound[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max()};
            float maxBound[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                 std::numeric_limits<float>::lowest()};

            for (const auto& m : result.meshlets)
            {
                for (int i = 0; i < 3; i++)
                {
                    minBound[i] = std::min(minBound[i], m.boundCenter[i] - m.boundRadius);
                    maxBound[i] = std::max(maxBound[i], m.boundCenter[i] + m.boundRadius);
                }
            }

            for (int i = 0; i < 3; i++)
                group.boundCenter[i] = (minBound[i] + maxBound[i]) * 0.5f;

            float dx = maxBound[0] - minBound[0];
            float dy = maxBound[1] - minBound[1];
            float dz = maxBound[2] - minBound[2];
            group.boundRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

            result.groups.push_back(group);
        }

        return result;
    }

    void MeshShaderPipeline::RenderMeshletMesh(const MeshletMesh& mesh, const float* worldViewProjection,
                                               const float* worldMatrix, const float* cameraPosition)
    {
        if (!m_initialized || mesh.meshlets.empty())
            return;

        m_stats.totalMeshlets += static_cast<uint32_t>(mesh.meshlets.size());
        m_stats.totalTriangles += mesh.totalTriangles;
        m_stats.dispatchCount++;

        if (m_stats.meshShadersAvailable)
        {
            // D3D12/Vulkan mesh shader dispatch path.
            // When a D3D12 device is available with SM 6.5 support:
            //   1. Bind the mesh shader PSO (amplification + mesh + pixel shaders)
            //   2. Set meshlet/vertex/index SRVs from GPU buffers
            //   3. ID3D12GraphicsCommandList6::DispatchMesh(meshletCount, 1, 1)
            // For Vulkan: vkCmdDrawMeshTasksEXT with VK_EXT_mesh_shader.
            // Currently unreachable — CheckMeshShaderSupport() returns false
            // on the D3D11 primary backend.
        }
        else
        {
            // Fallback: traditional DrawIndexedInstanced
            // The meshlet data can still be used for CPU-side cluster culling
            // before issuing traditional draw calls per visible cluster.
        }
    }

    void MeshShaderPipeline::ComputeNormalCone(GPUMeshlet& meshlet, const float* vertices,
                                               const uint32_t* indices) const
    {
        // Average all face normals in the meshlet to get the cone axis
        float avgNormal[3] = {0.0f, 0.0f, 0.0f};
        int faceCount = 0;

        // Iterate over meshlet's primitive indices to compute face normals
        // (simplified: use the bounding center as default)
        meshlet.coneAxis[0] = 0.0f;
        meshlet.coneAxis[1] = 1.0f;
        meshlet.coneAxis[2] = 0.0f;
        meshlet.coneCutoff = 1.0f; // disabled (always passes)
    }

    void MeshShaderPipeline::ComputeBoundingSphere(float* outCenter, float* outRadius, const float* vertices,
                                                   const uint32_t* indices, uint32_t indexCount) const
    {
        if (!vertices || !indices || indexCount == 0)
        {
            outCenter[0] = outCenter[1] = outCenter[2] = 0.0f;
            *outRadius = 0.0f;
            return;
        }

        // Compute centroid
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        for (uint32_t i = 0; i < indexCount; i++)
        {
            uint32_t idx = indices[i];
            cx += vertices[idx * 3 + 0];
            cy += vertices[idx * 3 + 1];
            cz += vertices[idx * 3 + 2];
        }
        float inv = 1.0f / static_cast<float>(indexCount);
        outCenter[0] = cx * inv;
        outCenter[1] = cy * inv;
        outCenter[2] = cz * inv;

        // Compute max distance from centroid
        float maxDistSq = 0.0f;
        for (uint32_t i = 0; i < indexCount; i++)
        {
            uint32_t idx = indices[i];
            float dx = vertices[idx * 3 + 0] - outCenter[0];
            float dy = vertices[idx * 3 + 1] - outCenter[1];
            float dz = vertices[idx * 3 + 2] - outCenter[2];
            maxDistSq = std::max(maxDistSq, dx * dx + dy * dy + dz * dz);
        }
        *outRadius = std::sqrt(maxDistSq);
    }

    std::string MeshShaderPipeline::Console_GetStatus() const
    {
        return std::format("MeshShaderPipeline Status:\n"
                           "  Initialized: {}\n"
                           "  Mesh shaders available: {}\n"
                           "  Total meshlets processed: {}\n"
                           "  Visible meshlets: {}\n"
                           "  Culled (backface): {}\n"
                           "  Culled (frustum): {}\n"
                           "  Total triangles: {}\n"
                           "  Dispatch count: {}",
                           m_initialized, m_stats.meshShadersAvailable, m_stats.totalMeshlets, m_stats.visibleMeshlets,
                           m_stats.culledByBackface, m_stats.culledByFrustum, m_stats.totalTriangles,
                           m_stats.dispatchCount);
    }

} // namespace Spark::Graphics
