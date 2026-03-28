// TestGPUClusterCulling.cpp - Tests for GPU clustered light culling
// Validates light-to-cluster assignment and sphere-AABB intersection

#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

    struct ClusterAABB
    {
        float minPoint[3];
        float maxPoint[3];
    };

    struct LightData
    {
        float position[3];
        float radius;
        uint32_t type; // 0=point, 1=spot, 2=directional
    };

    // CPU reference sphere-AABB intersection (mirrors ClusterCull.hlsl)
    bool SphereAABBIntersect(const float sphereCenter[3], float sphereRadius, const float aabbMin[3],
                             const float aabbMax[3])
    {
        float closestX = std::clamp(sphereCenter[0], aabbMin[0], aabbMax[0]);
        float closestY = std::clamp(sphereCenter[1], aabbMin[1], aabbMax[1]);
        float closestZ = std::clamp(sphereCenter[2], aabbMin[2], aabbMax[2]);

        float dx = sphereCenter[0] - closestX;
        float dy = sphereCenter[1] - closestY;
        float dz = sphereCenter[2] - closestZ;
        float distSq = dx * dx + dy * dy + dz * dz;

        return distSq <= (sphereRadius * sphereRadius);
    }

    // Assign lights to a single cluster
    std::vector<uint32_t> AssignLightsToCluster(const ClusterAABB& cluster, const std::vector<LightData>& lights)
    {
        std::vector<uint32_t> result;
        for (uint32_t i = 0; i < lights.size(); i++)
        {
            if (lights[i].type == 2) // directional always affects all clusters
            {
                result.push_back(i);
                continue;
            }

            if (SphereAABBIntersect(lights[i].position, lights[i].radius, cluster.minPoint, cluster.maxPoint))
            {
                result.push_back(i);
            }
        }
        return result;
    }

    struct ClusterGridConfig
    {
        uint32_t gridX = 16;
        uint32_t gridY = 8;
        uint32_t gridZ = 24;
        uint32_t maxLightsPerCluster = 200;
    };

} // namespace

TEST(ClusterCull_SphereAABB_Inside)
{
    float center[3] = {0, 0, 0};
    float aabbMin[3] = {-1, -1, -1};
    float aabbMax[3] = {1, 1, 1};

    EXPECT_TRUE(SphereAABBIntersect(center, 0.5f, aabbMin, aabbMax));
}

TEST(ClusterCull_SphereAABB_Outside)
{
    float center[3] = {5, 0, 0};
    float aabbMin[3] = {-1, -1, -1};
    float aabbMax[3] = {1, 1, 1};

    EXPECT_FALSE(SphereAABBIntersect(center, 1.0f, aabbMin, aabbMax));
}

TEST(ClusterCull_SphereAABB_Touching)
{
    float center[3] = {2, 0, 0};
    float aabbMin[3] = {-1, -1, -1};
    float aabbMax[3] = {1, 1, 1};

    EXPECT_TRUE(SphereAABBIntersect(center, 1.0f, aabbMin, aabbMax)); // exactly touching
}

TEST(ClusterCull_SphereAABB_Corner)
{
    float center[3] = {2, 2, 2};
    float aabbMin[3] = {0, 0, 0};
    float aabbMax[3] = {1, 1, 1};

    // Distance from corner (1,1,1) to sphere center (2,2,2) = sqrt(3) ~= 1.732
    EXPECT_FALSE(SphereAABBIntersect(center, 1.5f, aabbMin, aabbMax));
    EXPECT_TRUE(SphereAABBIntersect(center, 2.0f, aabbMin, aabbMax));
}

TEST(ClusterCull_PointLight_InsideCluster)
{
    ClusterAABB cluster = {{-1, -1, -5}, {1, 1, -3}};
    std::vector<LightData> lights = {{{0, 0, -4}, 0.5f, 0}};

    auto indices = AssignLightsToCluster(cluster, lights);
    EXPECT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], 0u);
}

TEST(ClusterCull_PointLight_OutsideCluster)
{
    ClusterAABB cluster = {{-1, -1, -5}, {1, 1, -3}};
    std::vector<LightData> lights = {{{10, 0, -4}, 0.5f, 0}};

    auto indices = AssignLightsToCluster(cluster, lights);
    EXPECT_EQ(indices.size(), 0u);
}

TEST(ClusterCull_DirectionalLight_AlwaysAssigned)
{
    ClusterAABB cluster = {{-1, -1, -5}, {1, 1, -3}};
    std::vector<LightData> lights = {{{100, 100, 100}, 0.0f, 2}}; // directional

    auto indices = AssignLightsToCluster(cluster, lights);
    EXPECT_EQ(indices.size(), 1u);
}

TEST(ClusterCull_MultipleLights_MixedResults)
{
    ClusterAABB cluster = {{0, 0, 0}, {2, 2, 2}};
    std::vector<LightData> lights = {
        {{1, 1, 1}, 0.5f, 0},  // inside -> assigned
        {{10, 0, 0}, 1.0f, 0}, // outside -> not assigned
        {{3, 1, 1}, 2.0f, 0},  // overlapping -> assigned
        {{0, 0, 0}, 0.0f, 2},  // directional -> assigned
    };

    auto indices = AssignLightsToCluster(cluster, lights);
    EXPECT_EQ(indices.size(), 3u);
}

TEST(ClusterCull_GridConfig_Defaults)
{
    ClusterGridConfig config;
    EXPECT_EQ(config.gridX, 16u);
    EXPECT_EQ(config.gridY, 8u);
    EXPECT_EQ(config.gridZ, 24u);
    EXPECT_EQ(config.maxLightsPerCluster, 200u);

    uint32_t totalClusters = config.gridX * config.gridY * config.gridZ;
    EXPECT_EQ(totalClusters, 3072u);
}

TEST(ClusterCull_GridConfig_MemoryEstimate)
{
    ClusterGridConfig config;
    uint32_t totalClusters = config.gridX * config.gridY * config.gridZ;
    uint32_t totalIndices = totalClusters * config.maxLightsPerCluster;

    // Memory = indices * sizeof(uint32_t) + counts * sizeof(uint32_t)
    size_t memoryBytes = totalIndices * sizeof(uint32_t) + totalClusters * sizeof(uint32_t);

    // Should be reasonable (< 10 MB for default config)
    EXPECT_TRUE(memoryBytes < 10 * 1024 * 1024);
}

TEST(ClusterCull_EmptyLightList_NothingAssigned)
{
    ClusterAABB cluster = {{0, 0, 0}, {1, 1, 1}};
    std::vector<LightData> lights;

    auto indices = AssignLightsToCluster(cluster, lights);
    EXPECT_EQ(indices.size(), 0u);
}
