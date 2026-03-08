// TestMeshLOD.cpp - Tests for LOD selection and mesh simplification logic
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace TestLOD
{

    struct LODLevel
    {
        float maxDistance = 0.0f;
        float screenSizeThreshold = 0.0f;
        uint32_t indexCount = 0;
        float qualityPercent = 0.0f;
    };

    struct LODChain
    {
        std::string meshName;
        std::vector<LODLevel> levels;

        int SelectLOD(float distance, float screenSize = -1.0f) const
        {
            if (levels.empty())
                return 0;

            // Distance-based selection
            for (int i = static_cast<int>(levels.size()) - 1; i >= 0; --i)
            {
                if (distance >= levels[i].maxDistance && levels[i].maxDistance > 0.0f)
                {
                    return i;
                }
            }

            // Screen-size-based fallback
            if (screenSize >= 0.0f)
            {
                for (int i = static_cast<int>(levels.size()) - 1; i >= 0; --i)
                {
                    if (screenSize <= levels[i].screenSizeThreshold)
                    {
                        return i;
                    }
                }
            }

            return 0;
        }
    };

    // Simplified edge cost for testing
    float ComputeEdgeCost(float px0, float py0, float pz0, float px1, float py1, float pz1, float nx0, float ny0,
                          float nz0, float nx1, float ny1, float nz1)
    {
        float dx = px1 - px0, dy = py1 - py0, dz = pz1 - pz0;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float normalDot = nx0 * nx1 + ny0 * ny1 + nz0 * nz1;
        float normalPenalty = 1.0f - (std::max)(0.0f, normalDot);
        return dist + normalPenalty * 2.0f;
    }

} // namespace TestLOD

TEST(LODSelection_ClosestDistance)
{
    TestLOD::LODChain chain;
    chain.levels = {
        {0.0f, 1.0f, 1000, 100.0f}, // LOD 0 (highest quality)
        {20.0f, 0.5f, 500, 50.0f},  // LOD 1
        {40.0f, 0.25f, 250, 25.0f}, // LOD 2
        {80.0f, 0.1f, 100, 10.0f}   // LOD 3 (lowest quality)
    };

    EXPECT_EQ(chain.SelectLOD(5.0f), 0);
}

TEST(LODSelection_MidDistance)
{
    TestLOD::LODChain chain;
    chain.levels = {
        {0.0f, 1.0f, 1000, 100.0f}, {20.0f, 0.5f, 500, 50.0f}, {40.0f, 0.25f, 250, 25.0f}, {80.0f, 0.1f, 100, 10.0f}};

    EXPECT_EQ(chain.SelectLOD(25.0f), 1);
}

TEST(LODSelection_FarDistance)
{
    TestLOD::LODChain chain;
    chain.levels = {
        {0.0f, 1.0f, 1000, 100.0f}, {20.0f, 0.5f, 500, 50.0f}, {40.0f, 0.25f, 250, 25.0f}, {80.0f, 0.1f, 100, 10.0f}};

    EXPECT_EQ(chain.SelectLOD(100.0f), 3);
}

TEST(LODSelection_EmptyChain)
{
    TestLOD::LODChain chain;
    EXPECT_EQ(chain.SelectLOD(50.0f), 0);
}

TEST(LODSelection_SingleLevel)
{
    TestLOD::LODChain chain;
    chain.levels = {{0.0f, 1.0f, 1000, 100.0f}};
    EXPECT_EQ(chain.SelectLOD(0.0f), 0);
    EXPECT_EQ(chain.SelectLOD(100.0f), 0);
}

TEST(EdgeCost_IdenticalVertices)
{
    float cost = TestLOD::ComputeEdgeCost(0, 0, 0, 0, 0, 0, // same position
                                          0, 1, 0, 0, 1, 0  // same normal
    );
    EXPECT_NEAR(cost, 0.0f, 0.001f);
}

TEST(EdgeCost_DistantVertices)
{
    float cost = TestLOD::ComputeEdgeCost(0, 0, 0, 10, 0, 0, // far apart
                                          0, 1, 0, 0, 1, 0   // same normal
    );
    EXPECT_NEAR(cost, 10.0f, 0.001f);
}

TEST(EdgeCost_OppositeNormals)
{
    float cost = TestLOD::ComputeEdgeCost(0, 0, 0, 0, 0, 0, // same position
                                          0, 1, 0, 0, -1, 0 // opposite normals
    );
    // normalDot = -1, penalty = 1 - max(0, -1) = 1.0, * 2.0 = 2.0
    // dist = 0
    EXPECT_NEAR(cost, 2.0f, 0.5f);
}
