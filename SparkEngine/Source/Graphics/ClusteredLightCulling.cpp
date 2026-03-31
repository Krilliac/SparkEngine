/**
 * @file ClusteredLightCulling.cpp
 * @brief Implementation of CPU-side clustered light culling
 * @author Spark Engine Team
 * @date 2026
 */

#include "ClusteredLightCulling.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Spark::Graphics
{

    bool ClusteredLightCulling::Initialize(const ClusterConfig& config)
    {
        if (m_initialized)
        {
            Shutdown();
        }

        m_config = config;

        uint32_t clusterCount = GetClusterCount();
        m_clusterAABBs.resize(clusterCount);
        m_clusterLightCounts.resize(clusterCount, 0);
        m_clusterLightIndices.resize(clusterCount * m_config.maxLightsPerCluster, 0);

        m_lights.reserve(m_config.maxTotalLights);
        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ClusteredLightCulling initialized (grid=%ux%ux%u, %u clusters)",
                       config.gridX, config.gridY, config.gridZ, clusterCount);
        return true;
    }

    void ClusteredLightCulling::Update(const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix, float nearZ,
                                       float farZ)
    {
        if (!m_initialized)
        {
            return;
        }

        RebuildClusterGrid(projMatrix, nearZ, farZ);
        AssignLightsToClusters(viewMatrix);
    }

    void ClusteredLightCulling::AddLight(const LightData& light)
    {
        if (!m_initialized)
        {
            return;
        }

        if (m_lights.size() < m_config.maxTotalLights)
        {
            m_lights.push_back(light);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "ClusteredLightCulling: light overflow, max %u reached",
                           m_config.maxTotalLights);
        }
    }

    void ClusteredLightCulling::ClearLights()
    {
        m_lights.clear();
    }

    uint32_t ClusteredLightCulling::GetClusterCount() const
    {
        return m_config.gridX * m_config.gridY * m_config.gridZ;
    }

    uint32_t ClusteredLightCulling::GetActiveLightCount() const
    {
        return static_cast<uint32_t>(m_lights.size());
    }

    const ClusterConfig& ClusteredLightCulling::GetConfig() const
    {
        return m_config;
    }

    uint32_t ClusteredLightCulling::GetClusterLightCount(uint32_t clusterIndex) const
    {
        if (clusterIndex >= GetClusterCount())
        {
            return 0;
        }
        return m_clusterLightCounts[clusterIndex];
    }

    const uint32_t* ClusteredLightCulling::GetClusterLightIndices(uint32_t clusterIndex) const
    {
        if (clusterIndex >= GetClusterCount())
        {
            return nullptr;
        }
        return &m_clusterLightIndices[clusterIndex * m_config.maxLightsPerCluster];
    }

    std::string ClusteredLightCulling::Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return "ClusteredLightCulling: Not initialized\n";
        }

        uint32_t totalAssigned = 0;
        uint32_t maxPerCluster = 0;
        uint32_t clustersWithLights = 0;

        for (uint32_t i = 0; i < GetClusterCount(); ++i)
        {
            uint32_t count = m_clusterLightCounts[i];
            totalAssigned += count;
            maxPerCluster = std::max(maxPerCluster, count);
            if (count > 0)
            {
                ++clustersWithLights;
            }
        }

        std::string status = "ClusteredLightCulling:\n";
        status += std::format("  Grid: {}x{}x{} ({} clusters)\n", m_config.gridX, m_config.gridY, m_config.gridZ,
                              GetClusterCount());
        status += std::format("  Active lights: {}/{}\n", m_lights.size(), m_config.maxTotalLights);
        status += std::format("  Clusters with lights: {}\n", clustersWithLights);
        status += std::format("  Max lights in one cluster: {}\n", maxPerCluster);
        status += std::format("  Total light-cluster assignments: {}\n", totalAssigned);

        return status;
    }

    void ClusteredLightCulling::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ClusteredLightCulling shutting down");
        m_lights.clear();
        m_clusterAABBs.clear();
        m_clusterLightCounts.clear();
        m_clusterLightIndices.clear();
        m_initialized = false;
    }

    void ClusteredLightCulling::RebuildClusterGrid(const XMFLOAT4X4& projMatrix, float nearZ, float farZ)
    {
        // Use logarithmic depth slicing for better distribution of clusters along Z.
        // Each cluster's near/far are computed in view space, then the X/Y extents
        // are derived from the projection matrix to form an AABB in view space.

        float logNear = std::log(std::max(nearZ, 0.001f));
        float logFar = std::log(std::max(farZ, nearZ + 0.001f));
        float logRange = logFar - logNear;

        // Extract projection parameters for X and Y extent computation.
        // proj._11 = 1 / (aspect * tan(fovY/2)), proj._22 = 1 / tan(fovY/2)
        float invProjX = (projMatrix.m[0][0] != 0.0f) ? (1.0f / projMatrix.m[0][0]) : 1.0f;
        float invProjY = (projMatrix.m[1][1] != 0.0f) ? (1.0f / projMatrix.m[1][1]) : 1.0f;

        for (uint32_t z = 0; z < m_config.gridZ; ++z)
        {
            // Logarithmic depth slice boundaries
            float sliceNear = std::exp(logNear + logRange * static_cast<float>(z) / static_cast<float>(m_config.gridZ));
            float sliceFar =
                std::exp(logNear + logRange * static_cast<float>(z + 1) / static_cast<float>(m_config.gridZ));

            for (uint32_t y = 0; y < m_config.gridY; ++y)
            {
                for (uint32_t x = 0; x < m_config.gridX; ++x)
                {
                    uint32_t idx = x + y * m_config.gridX + z * m_config.gridX * m_config.gridY;

                    // Normalized tile coordinates [0, 1]
                    float tileMinX = static_cast<float>(x) / static_cast<float>(m_config.gridX);
                    float tileMaxX = static_cast<float>(x + 1) / static_cast<float>(m_config.gridX);
                    float tileMinY = static_cast<float>(y) / static_cast<float>(m_config.gridY);
                    float tileMaxY = static_cast<float>(y + 1) / static_cast<float>(m_config.gridY);

                    // Map [0,1] to NDC [-1,1]
                    float ndcMinX = tileMinX * 2.0f - 1.0f;
                    float ndcMaxX = tileMaxX * 2.0f - 1.0f;
                    float ndcMinY = tileMinY * 2.0f - 1.0f;
                    float ndcMaxY = tileMaxY * 2.0f - 1.0f;

                    // View-space extents at the far slice depth (conservative AABB)
                    float viewMinX = ndcMinX * invProjX * sliceFar;
                    float viewMaxX = ndcMaxX * invProjX * sliceFar;
                    float viewMinY = ndcMinY * invProjY * sliceFar;
                    float viewMaxY = ndcMaxY * invProjY * sliceFar;

                    auto& aabb = m_clusterAABBs[idx];
                    aabb.minBounds = {std::min(viewMinX, viewMaxX), std::min(viewMinY, viewMaxY), sliceNear};
                    aabb.maxBounds = {std::max(viewMinX, viewMaxX), std::max(viewMinY, viewMaxY), sliceFar};
                }
            }
        }
    }

    void ClusteredLightCulling::AssignLightsToClusters(const XMFLOAT4X4& viewMatrix)
    {
        // Reset all cluster light counts
        std::fill(m_clusterLightCounts.begin(), m_clusterLightCounts.end(), 0);

        for (uint32_t lightIdx = 0; lightIdx < static_cast<uint32_t>(m_lights.size()); ++lightIdx)
        {
            const auto& light = m_lights[lightIdx];

            // Transform light position into view space
            XMFLOAT3 viewPos = TransformPoint(light.position, viewMatrix);

            // --- Z-slice range culling ---
            // Determine which Z slices the light's bounding sphere can possibly
            // overlap based on the Z (depth) extent. This turns O(gridX*gridY*gridZ)
            // into O(gridX*gridY*affectedSlices) per light — typically 10-100x fewer
            // intersection tests for point lights that affect only a few depth slices.
            float lightZMin = viewPos.z - light.radius;
            float lightZMax = viewPos.z + light.radius;

            uint32_t zStart = 0;
            uint32_t zEnd = m_config.gridZ;

            // Find the first Z slice whose far bound is >= lightZMin
            for (uint32_t z = 0; z < m_config.gridZ; ++z)
            {
                uint32_t sampleIdx = z * m_config.gridX * m_config.gridY; // First cluster in slice z
                if (m_clusterAABBs[sampleIdx].maxBounds.z >= lightZMin)
                {
                    zStart = z;
                    break;
                }
            }
            // Find the last Z slice whose near bound is <= lightZMax
            for (uint32_t z = m_config.gridZ; z > zStart; --z)
            {
                uint32_t sampleIdx = (z - 1) * m_config.gridX * m_config.gridY;
                if (m_clusterAABBs[sampleIdx].minBounds.z <= lightZMax)
                {
                    zEnd = z;
                    break;
                }
            }

            // Only test clusters in the affected Z-slice range
            for (uint32_t z = zStart; z < zEnd; ++z)
            {
                for (uint32_t y = 0; y < m_config.gridY; ++y)
                {
                    for (uint32_t x = 0; x < m_config.gridX; ++x)
                    {
                        uint32_t c = x + y * m_config.gridX + z * m_config.gridX * m_config.gridY;

                        if (m_clusterLightCounts[c] >= m_config.maxLightsPerCluster)
                        {
                            continue;
                        }

                        if (SphereIntersectsAABB(viewPos, light.radius, m_clusterAABBs[c]))
                        {
                            uint32_t offset = c * m_config.maxLightsPerCluster + m_clusterLightCounts[c];
                            m_clusterLightIndices[offset] = lightIdx;
                            ++m_clusterLightCounts[c];
                        }
                    }
                }
            }
        }
    }

    bool ClusteredLightCulling::SphereIntersectsAABB(const XMFLOAT3& center, float radius, const ClusterAABB& aabb)
    {
        // Squared distance from sphere center to nearest AABB point
        float distSq = 0.0f;

        auto clampAxis = [](float val, float minVal, float maxVal, float& outDistSq)
        {
            if (val < minVal)
            {
                float d = minVal - val;
                outDistSq += d * d;
            }
            else if (val > maxVal)
            {
                float d = val - maxVal;
                outDistSq += d * d;
            }
        };

        clampAxis(center.x, aabb.minBounds.x, aabb.maxBounds.x, distSq);
        clampAxis(center.y, aabb.minBounds.y, aabb.maxBounds.y, distSq);
        clampAxis(center.z, aabb.minBounds.z, aabb.maxBounds.z, distSq);

        return distSq <= (radius * radius);
    }

    XMFLOAT3 ClusteredLightCulling::TransformPoint(const XMFLOAT3& point, const XMFLOAT4X4& matrix)
    {
        // Row-major 4x4 transform: result = point * matrix (homogeneous, w=1)
        float x = point.x * matrix.m[0][0] + point.y * matrix.m[1][0] + point.z * matrix.m[2][0] + matrix.m[3][0];
        float y = point.x * matrix.m[0][1] + point.y * matrix.m[1][1] + point.z * matrix.m[2][1] + matrix.m[3][1];
        float z = point.x * matrix.m[0][2] + point.y * matrix.m[1][2] + point.z * matrix.m[2][2] + matrix.m[3][2];
        float w = point.x * matrix.m[0][3] + point.y * matrix.m[1][3] + point.z * matrix.m[2][3] + matrix.m[3][3];

        if (w != 0.0f && w != 1.0f)
        {
            x /= w;
            y /= w;
            z /= w;
        }

        return {x, y, z};
    }

} // namespace Spark::Graphics
