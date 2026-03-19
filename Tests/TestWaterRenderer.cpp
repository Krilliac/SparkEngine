// TestWaterRenderer.cpp - Tests for water plane management, height queries, and update
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace TestWater
{

    using WaterID = uint32_t;

    struct Vec2
    {
        float x = 0.0f, y = 0.0f;
    };

    struct WaterPlane
    {
        WaterID id = 0;
        float baseHeight = 0.0f;
        float currentHeight = 0.0f;
        Vec2 center;
        Vec2 extents;
        float waveAmplitude = 0.1f;
        float waveFrequency = 1.0f;
    };

    struct WaterRenderer
    {
        std::unordered_map<WaterID, WaterPlane> planes;
        WaterID nextID = 1;
        bool initialized = false;
        float time = 0.0f;

        bool Initialize()
        {
            initialized = true;
            time = 0.0f;
            return true;
        }

        void Shutdown()
        {
            planes.clear();
            initialized = false;
        }

        WaterID AddWaterPlane(float height, Vec2 center, Vec2 extents, float waveAmp, float waveFreq)
        {
            if (!initialized)
                return 0;
            WaterID id = nextID++;
            planes[id] = {id, height, height, center, extents, waveAmp, waveFreq};
            return id;
        }

        bool RemoveWaterPlane(WaterID id) { return planes.erase(id) > 0; }

        float GetWaterHeight(WaterID id) const
        {
            auto it = planes.find(id);
            if (it == planes.end())
                return -1e30f;
            return it->second.currentHeight;
        }

        void Update(float dt)
        {
            if (!initialized)
                return;
            time += dt;
            for (auto& [id, plane] : planes)
            {
                plane.currentHeight = plane.baseHeight + plane.waveAmplitude * std::sin(time * plane.waveFrequency);
            }
        }
    };

} // namespace TestWater

// ============================================================================
// Initialization
// ============================================================================

TEST(WaterRenderer_InitializeAndShutdown)
{
    TestWater::WaterRenderer renderer;
    EXPECT_TRUE(renderer.Initialize());
    EXPECT_TRUE(renderer.initialized);
    renderer.Shutdown();
    EXPECT_FALSE(renderer.initialized);
}

// ============================================================================
// Water Plane Management
// ============================================================================

TEST(WaterRenderer_AddWaterPlane)
{
    TestWater::WaterRenderer renderer;
    renderer.Initialize();

    auto id = renderer.AddWaterPlane(10.0f, {0, 0}, {100, 100}, 0.5f, 2.0f);
    EXPECT_NE(id, (uint32_t)0);
    EXPECT_EQ(static_cast<int>(renderer.planes.size()), 1);

    renderer.Shutdown();
}

TEST(WaterRenderer_AddWaterPlane_NotInitialized)
{
    TestWater::WaterRenderer renderer;
    auto id = renderer.AddWaterPlane(10.0f, {0, 0}, {100, 100}, 0.5f, 2.0f);
    EXPECT_EQ(id, (uint32_t)0);
}

TEST(WaterRenderer_RemoveWaterPlane)
{
    TestWater::WaterRenderer renderer;
    renderer.Initialize();

    auto id = renderer.AddWaterPlane(5.0f, {0, 0}, {50, 50}, 0.2f, 1.0f);
    EXPECT_TRUE(renderer.RemoveWaterPlane(id));
    EXPECT_FALSE(renderer.RemoveWaterPlane(id));
    EXPECT_EQ(static_cast<int>(renderer.planes.size()), 0);

    renderer.Shutdown();
}

// ============================================================================
// Height Queries
// ============================================================================

TEST(WaterRenderer_GetWaterHeight)
{
    TestWater::WaterRenderer renderer;
    renderer.Initialize();

    auto id = renderer.AddWaterPlane(10.0f, {0, 0}, {100, 100}, 0.0f, 1.0f);
    EXPECT_NEAR(renderer.GetWaterHeight(id), 10.0f, 0.001f);

    // Invalid ID
    EXPECT_LT(renderer.GetWaterHeight(999), -1e20f);

    renderer.Shutdown();
}

// ============================================================================
// Update with Waves
// ============================================================================

TEST(WaterRenderer_Update)
{
    TestWater::WaterRenderer renderer;
    renderer.Initialize();

    auto id = renderer.AddWaterPlane(10.0f, {0, 0}, {100, 100}, 2.0f, 1.0f);
    EXPECT_NEAR(renderer.GetWaterHeight(id), 10.0f, 0.001f);

    // After some time, height should change due to waves
    renderer.Update(1.5f);
    float h = renderer.GetWaterHeight(id);
    // Height = 10.0 + 2.0 * sin(1.5 * 1.0)
    float expected = 10.0f + 2.0f * std::sin(1.5f);
    EXPECT_NEAR(h, expected, 0.01f);

    renderer.Shutdown();
}
