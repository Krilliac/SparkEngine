// TestOcclusionCulling.cpp - Tests for occlusion culling initialization, frame management, and visibility
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <vector>

namespace TestOcclusion
{

    struct AABB
    {
        float minX, minY, minZ;
        float maxX, maxY, maxZ;

        bool Overlaps(const AABB& other) const
        {
            return minX <= other.maxX && maxX >= other.minX && minY <= other.maxY && maxY >= other.minY &&
                   minZ <= other.maxZ && maxZ >= other.minZ;
        }
    };

    struct Occluder
    {
        AABB bounds;
    };

    struct OcclusionCullingSystem
    {
        std::vector<Occluder> occluders;
        bool initialized = false;
        bool inFrame = false;
        int queriesThisFrame = 0;
        int visibleCount = 0;
        int culledCount = 0;

        bool Initialize()
        {
            initialized = true;
            return true;
        }

        void Shutdown()
        {
            occluders.clear();
            initialized = false;
        }

        void AddOccluder(const AABB& bounds) { occluders.push_back({bounds}); }

        void BeginFrame()
        {
            inFrame = true;
            queriesThisFrame = 0;
            visibleCount = 0;
            culledCount = 0;
        }

        bool TestVisibility(const AABB& testBounds)
        {
            if (!initialized || !inFrame)
                return true; // Conservative: visible if system not ready

            queriesThisFrame++;

            // Simple occlusion: if the test bounds is fully behind any occluder
            // (simplified: if any occluder fully contains the test bounds on XZ and
            // the occluder is taller, mark as occluded)
            for (const auto& occ : occluders)
            {
                if (occ.bounds.minX <= testBounds.minX && occ.bounds.maxX >= testBounds.maxX &&
                    occ.bounds.minZ <= testBounds.minZ && occ.bounds.maxZ >= testBounds.maxZ &&
                    occ.bounds.maxY >= testBounds.maxY)
                {
                    culledCount++;
                    return false; // Occluded
                }
            }

            visibleCount++;
            return true;
        }

        void EndFrame() { inFrame = false; }
    };

} // namespace TestOcclusion

// ============================================================================
// Initialization
// ============================================================================

TEST(OcclusionCulling_InitializeAndShutdown)
{
    TestOcclusion::OcclusionCullingSystem sys;
    EXPECT_TRUE(sys.Initialize());
    EXPECT_TRUE(sys.initialized);
    sys.Shutdown();
    EXPECT_FALSE(sys.initialized);
}

// ============================================================================
// Frame Management
// ============================================================================

TEST(OcclusionCulling_BeginEndFrame)
{
    TestOcclusion::OcclusionCullingSystem sys;
    sys.Initialize();

    EXPECT_FALSE(sys.inFrame);
    sys.BeginFrame();
    EXPECT_TRUE(sys.inFrame);
    sys.EndFrame();
    EXPECT_FALSE(sys.inFrame);

    sys.Shutdown();
}

// ============================================================================
// Visibility Testing
// ============================================================================

TEST(OcclusionCulling_TestVisibility_NoOccluders)
{
    TestOcclusion::OcclusionCullingSystem sys;
    sys.Initialize();
    sys.BeginFrame();

    bool visible = sys.TestVisibility({5, 0, 5, 6, 2, 6});
    EXPECT_TRUE(visible);
    EXPECT_EQ(sys.visibleCount, 1);

    sys.EndFrame();
    sys.Shutdown();
}

TEST(OcclusionCulling_TestVisibility_Occluded)
{
    TestOcclusion::OcclusionCullingSystem sys;
    sys.Initialize();

    // Large occluder covering a wide area
    sys.AddOccluder({0, 0, 0, 20, 10, 20});

    sys.BeginFrame();
    // Small object fully within the occluder's XZ footprint and shorter
    bool visible = sys.TestVisibility({5, 0, 5, 6, 2, 6});
    EXPECT_FALSE(visible);
    EXPECT_EQ(sys.culledCount, 1);

    sys.EndFrame();
    sys.Shutdown();
}

TEST(OcclusionCulling_TestVisibility_PartiallyOutside)
{
    TestOcclusion::OcclusionCullingSystem sys;
    sys.Initialize();

    // Occluder covers 0-10 on X
    sys.AddOccluder({0, 0, 0, 10, 10, 10});

    sys.BeginFrame();
    // Object extends beyond occluder on X axis
    bool visible = sys.TestVisibility({5, 0, 0, 15, 5, 5});
    EXPECT_TRUE(visible);
    EXPECT_EQ(sys.visibleCount, 1);

    sys.EndFrame();
    sys.Shutdown();
}

TEST(OcclusionCulling_TestVisibility_NotInitialized)
{
    TestOcclusion::OcclusionCullingSystem sys;
    // Not initialized — should return visible (conservative)
    bool visible = sys.TestVisibility({0, 0, 0, 1, 1, 1});
    EXPECT_TRUE(visible);
}
