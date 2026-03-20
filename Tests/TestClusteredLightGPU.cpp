// TestClusteredLightGPU.cpp - Tests for clustered forward+ GPU buffer layout and constants

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <vector>

// Standalone reproduction of ClusteredLightGPU structs for CI testing
namespace TestClustered
{

    struct GPULightData
    {
        float positionX, positionY, positionZ;
        float radius;
        float colorR, colorG, colorB;
        float intensity;
        uint32_t type;
        float pad0, pad1, pad2;
    };

    struct ClusterLightGrid
    {
        uint32_t offset;
        uint32_t count;
    };

    struct ClusteredLightGPUData
    {
        std::vector<GPULightData> lights;
        std::vector<ClusterLightGrid> grid;
        std::vector<uint32_t> lightIndexList;
        uint32_t clusterCountX = 0;
        uint32_t clusterCountY = 0;
        uint32_t clusterCountZ = 0;
        uint32_t totalClusters = 0;
        uint32_t activeLightCount = 0;
    };

    struct alignas(16) ClusteredLightConstants
    {
        uint32_t clusterCountX;
        uint32_t clusterCountY;
        uint32_t clusterCountZ;
        uint32_t totalClusters;
        float nearZ;
        float farZ;
        float logFarOverNear;
        uint32_t activeLightCount;
    };

    inline ClusteredLightConstants BuildConstants(uint32_t gridX, uint32_t gridY, uint32_t gridZ, float nearZ,
                                                  float farZ, uint32_t lightCount)
    {
        ClusteredLightConstants cb{};
        cb.clusterCountX = gridX;
        cb.clusterCountY = gridY;
        cb.clusterCountZ = gridZ;
        cb.totalClusters = gridX * gridY * gridZ;
        cb.nearZ = nearZ;
        cb.farZ = farZ;
        cb.logFarOverNear = (farZ > nearZ && nearZ > 0.0f) ? std::log2(farZ / nearZ) : 1.0f;
        cb.activeLightCount = lightCount;
        return cb;
    }

} // namespace TestClustered

TEST(ClusteredLightGPU_GPULightDataSize)
{
    EXPECT_EQ(sizeof(TestClustered::GPULightData), static_cast<size_t>(48));
}

TEST(ClusteredLightGPU_ClusterLightGridSize)
{
    EXPECT_EQ(sizeof(TestClustered::ClusterLightGrid), static_cast<size_t>(8));
}

TEST(ClusteredLightGPU_ConstantsLayout)
{
    EXPECT_EQ(sizeof(TestClustered::ClusteredLightConstants), static_cast<size_t>(32));
    EXPECT_GE(alignof(TestClustered::ClusteredLightConstants), static_cast<size_t>(16));
}

TEST(ClusteredLightGPU_BuildConstants)
{
    auto cb = TestClustered::BuildConstants(16, 9, 24, 0.1f, 1000.0f, 42);

    EXPECT_EQ(cb.clusterCountX, 16u);
    EXPECT_EQ(cb.clusterCountY, 9u);
    EXPECT_EQ(cb.clusterCountZ, 24u);
    EXPECT_EQ(cb.totalClusters, 16u * 9u * 24u);
    EXPECT_NEAR(cb.nearZ, 0.1f, 1e-6f);
    EXPECT_NEAR(cb.farZ, 1000.0f, 1e-6f);
    EXPECT_EQ(cb.activeLightCount, 42u);

    float expected = std::log2(1000.0f / 0.1f);
    EXPECT_NEAR(cb.logFarOverNear, expected, 0.001f);
}

TEST(ClusteredLightGPU_BuildConstantsEdgeCases)
{
    // nearZ == 0 should produce fallback
    auto cb1 = TestClustered::BuildConstants(1, 1, 1, 0.0f, 100.0f, 0);
    EXPECT_NEAR(cb1.logFarOverNear, 1.0f, 1e-6f);

    // farZ <= nearZ should produce fallback
    auto cb2 = TestClustered::BuildConstants(1, 1, 1, 10.0f, 5.0f, 0);
    EXPECT_NEAR(cb2.logFarOverNear, 1.0f, 1e-6f);
}

TEST(ClusteredLightGPU_GPUDataDefaults)
{
    TestClustered::ClusteredLightGPUData data;
    EXPECT_EQ(data.clusterCountX, 0u);
    EXPECT_EQ(data.clusterCountY, 0u);
    EXPECT_EQ(data.clusterCountZ, 0u);
    EXPECT_EQ(data.totalClusters, 0u);
    EXPECT_EQ(data.activeLightCount, 0u);
    EXPECT_TRUE(data.lights.empty());
    EXPECT_TRUE(data.grid.empty());
    EXPECT_TRUE(data.lightIndexList.empty());
}

TEST(ClusteredLightGPU_GPULightDataFields)
{
    TestClustered::GPULightData light{};
    light.positionX = 1.0f;
    light.positionY = 2.0f;
    light.positionZ = 3.0f;
    light.radius = 10.0f;
    light.colorR = 0.5f;
    light.colorG = 0.6f;
    light.colorB = 0.7f;
    light.intensity = 2.0f;
    light.type = 1;

    EXPECT_NEAR(light.positionX, 1.0f, 1e-6f);
    EXPECT_NEAR(light.radius, 10.0f, 1e-6f);
    EXPECT_EQ(light.type, 1u);
    EXPECT_NEAR(light.intensity, 2.0f, 1e-6f);
}
