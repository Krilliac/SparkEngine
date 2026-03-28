// TestGPUDrivenRenderer.cpp - Tests for GPU-driven indirect rendering pipeline
// Validates frustum culling, HiZ occlusion, and indirect args without D3D11

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

    struct AABB
    {
        float center[3];
        float extents[3];
    };

    struct FrustumPlane
    {
        float normal[3];
        float distance;
    };

    // CPU reference frustum test (mirrors GPUCull.hlsl)
    bool FrustumTest(const AABB& aabb, const FrustumPlane planes[6])
    {
        for (int i = 0; i < 6; i++)
        {
            float r = aabb.extents[0] * std::abs(planes[i].normal[0]) +
                      aabb.extents[1] * std::abs(planes[i].normal[1]) + aabb.extents[2] * std::abs(planes[i].normal[2]);

            float d = planes[i].normal[0] * aabb.center[0] + planes[i].normal[1] * aabb.center[1] +
                      planes[i].normal[2] * aabb.center[2] + planes[i].distance;

            if (d + r < 0.0f)
                return false; // outside
        }
        return true; // inside or intersecting
    }

    // Simple HiZ test: is object occluded by a maximum depth value?
    bool HiZTest(float objectNearDepth, float hiZMaxDepth)
    {
        return objectNearDepth <= hiZMaxDepth; // visible if closer than max depth
    }

    // Build trivial frustum looking down -Z, covering [-1,1] in XY at z=0..100
    void BuildSimpleFrustum(FrustumPlane planes[6])
    {
        // Left: x > -1
        planes[0] = {{1, 0, 0}, 1.0f};
        // Right: x < 1
        planes[1] = {{-1, 0, 0}, 1.0f};
        // Bottom: y > -1
        planes[2] = {{0, 1, 0}, 1.0f};
        // Top: y < 1
        planes[3] = {{0, -1, 0}, 1.0f};
        // Near: z > 0
        planes[4] = {{0, 0, 1}, 0.0f};
        // Far: z < 100
        planes[5] = {{0, 0, -1}, 100.0f};
    }

    struct IndirectDrawArgs
    {
        uint32_t indexCountPerInstance;
        uint32_t instanceCount;
        uint32_t startIndexLocation;
        int32_t baseVertexLocation;
        uint32_t startInstanceLocation;
    };

} // namespace

TEST(GPUDriven_FrustumCull_InsideBox)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    AABB inside = {{0, 0, 50}, {0.5f, 0.5f, 0.5f}};
    EXPECT_TRUE(FrustumTest(inside, planes));
}

TEST(GPUDriven_FrustumCull_OutsideLeft)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    AABB outside = {{-5, 0, 50}, {0.5f, 0.5f, 0.5f}};
    EXPECT_FALSE(FrustumTest(outside, planes));
}

TEST(GPUDriven_FrustumCull_OutsideRight)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    AABB outside = {{5, 0, 50}, {0.5f, 0.5f, 0.5f}};
    EXPECT_FALSE(FrustumTest(outside, planes));
}

TEST(GPUDriven_FrustumCull_OutsideBehind)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    AABB behind = {{0, 0, -5}, {0.5f, 0.5f, 0.5f}};
    EXPECT_FALSE(FrustumTest(behind, planes));
}

TEST(GPUDriven_FrustumCull_OutsideBeyondFar)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    AABB tooFar = {{0, 0, 200}, {0.5f, 0.5f, 0.5f}};
    EXPECT_FALSE(FrustumTest(tooFar, planes));
}

TEST(GPUDriven_FrustumCull_Intersecting)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    // Box straddles the left boundary
    AABB straddling = {{-1, 0, 50}, {0.5f, 0.5f, 0.5f}};
    EXPECT_TRUE(FrustumTest(straddling, planes)); // intersecting = visible
}

TEST(GPUDriven_HiZ_Visible)
{
    // Object at depth 0.3, HiZ max = 0.5 -> visible (closer)
    EXPECT_TRUE(HiZTest(0.3f, 0.5f));
}

TEST(GPUDriven_HiZ_Occluded)
{
    // Object at depth 0.8, HiZ max = 0.5 -> occluded (behind)
    EXPECT_FALSE(HiZTest(0.8f, 0.5f));
}

TEST(GPUDriven_HiZ_ExactBoundary)
{
    // Object at exact HiZ depth -> visible (not strictly behind)
    EXPECT_TRUE(HiZTest(0.5f, 0.5f));
}

TEST(GPUDriven_IndirectArgs_Layout)
{
    EXPECT_EQ(sizeof(IndirectDrawArgs), 20u);

    IndirectDrawArgs args = {};
    args.indexCountPerInstance = 36;
    args.instanceCount = 0; // set by GPU
    args.startIndexLocation = 0;
    args.baseVertexLocation = 0;
    args.startInstanceLocation = 0;

    EXPECT_EQ(args.indexCountPerInstance, 36u);
}

TEST(GPUDriven_BatchCull_CountsCorrectly)
{
    FrustumPlane planes[6];
    BuildSimpleFrustum(planes);

    std::vector<AABB> instances = {
        {{0, 0, 10}, {0.5f, 0.5f, 0.5f}},    // visible
        {{-5, 0, 10}, {0.5f, 0.5f, 0.5f}},   // culled (left)
        {{0, 0, 50}, {0.5f, 0.5f, 0.5f}},    // visible
        {{0, 0, 200}, {0.5f, 0.5f, 0.5f}},   // culled (far)
        {{0.5f, 0, 30}, {0.5f, 0.5f, 0.5f}}, // visible
    };

    uint32_t visibleCount = 0;
    std::vector<uint32_t> visibleIndices;

    for (uint32_t i = 0; i < instances.size(); i++)
    {
        if (FrustumTest(instances[i], planes))
        {
            visibleIndices.push_back(i);
            visibleCount++;
        }
    }

    EXPECT_EQ(visibleCount, 3u);
    EXPECT_EQ(visibleIndices[0], 0u);
    EXPECT_EQ(visibleIndices[1], 2u);
    EXPECT_EQ(visibleIndices[2], 4u);
}

TEST(GPUDriven_HiZMipSelection)
{
    // Mip level = ceil(log2(max(screenExtentX, screenExtentY)))
    // Object covers 64x32 pixels -> mip = ceil(log2(64)) = 6
    float extentX = 64.0f;
    float extentY = 32.0f;
    float mip = std::ceil(std::log2(std::max(extentX, extentY)));

    EXPECT_NEAR(mip, 6.0f, 0.001f);
}

TEST(GPUDriven_HiZMipSelection_SmallObject)
{
    // Object covers 2x2 pixels -> mip = ceil(log2(2)) = 1
    float mip = std::ceil(std::log2(2.0f));
    EXPECT_NEAR(mip, 1.0f, 0.001f);
}
