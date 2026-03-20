// TestParallelCulling.cpp - Tests for parallel frustum culling dispatch
// Validates that parallel AABB culling produces correct results across threads.

#include "TestFramework.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace TestParallelCulling
{

    // Simplified AABB / plane structs (platform-independent, no DirectXMath dependency)
    struct Vec3
    {
        float x, y, z;
    };

    struct AABB
    {
        Vec3 min{0, 0, 0};
        Vec3 max{0, 0, 0};
    };

    struct Plane
    {
        float a = 0, b = 0, c = 0, d = 0;

        void Normalize()
        {
            float len = std::sqrt(a * a + b * b + c * c);
            if (len > 0.0f)
            {
                float inv = 1.0f / len;
                a *= inv;
                b *= inv;
                c *= inv;
                d *= inv;
            }
        }

        float DistanceToPoint(const Vec3& p) const { return a * p.x + b * p.y + c * p.z + d; }
    };

    // Minimal frustum with 6 planes for testing
    class Frustum
    {
      public:
        static constexpr int kPlaneCount = 6;

        // Set up a simple axis-aligned frustum box for testing
        void SetupBox(float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
        {
            // Left plane: x >= minX  → normal (+1,0,0), d = -minX
            m_planes[0] = {1, 0, 0, -minX};
            // Right plane: x <= maxX → normal (-1,0,0), d = maxX
            m_planes[1] = {-1, 0, 0, maxX};
            // Bottom: y >= minY
            m_planes[2] = {0, 1, 0, -minY};
            // Top: y <= maxY
            m_planes[3] = {0, -1, 0, maxY};
            // Near: z >= minZ
            m_planes[4] = {0, 0, 1, -minZ};
            // Far: z <= maxZ
            m_planes[5] = {0, 0, -1, maxZ};
        }

        bool IsVisible(const AABB& box) const
        {
            for (int i = 0; i < kPlaneCount; ++i)
            {
                const Plane& p = m_planes[i];
                Vec3 pVertex{(p.a >= 0) ? box.max.x : box.min.x, (p.b >= 0) ? box.max.y : box.min.y,
                             (p.c >= 0) ? box.max.z : box.min.z};
                if (p.DistanceToPoint(pVertex) < 0.0f)
                {
                    return false;
                }
            }
            return true;
        }

        // Single-threaded batch cull
        uint32_t BatchCull(const AABB* boxes, uint32_t count, bool* outVisible) const
        {
            uint32_t visibleCount = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                outVisible[i] = IsVisible(boxes[i]);
                if (outVisible[i])
                    ++visibleCount;
            }
            return visibleCount;
        }

        // Parallel batch cull (mirrors FrustumCulling.h::ParallelBatchCull logic)
        uint32_t ParallelBatchCull(const AABB* boxes, uint32_t count, bool* outVisible,
                                   uint32_t minBatchSize = 64) const
        {
            if (count <= minBatchSize)
            {
                return BatchCull(boxes, count, outVisible);
            }

            std::atomic<uint32_t> totalVisible{0};
            int total = static_cast<int>(count);
            int batch = static_cast<int>(minBatchSize);

            // Simulate ParallelFor with real threads
            std::vector<std::future<void>> futures;
            for (int start = 0; start < total; start += batch)
            {
                int end = std::min(start + batch, total);
                futures.push_back(std::async(std::launch::async,
                                             [this, boxes, outVisible, &totalVisible, start, end]()
                                             {
                                                 for (int i = start; i < end; ++i)
                                                 {
                                                     bool visible = IsVisible(boxes[i]);
                                                     outVisible[i] = visible;
                                                     if (visible)
                                                         totalVisible.fetch_add(1, std::memory_order_relaxed);
                                                 }
                                             }));
            }
            for (auto& f : futures)
            {
                f.get();
            }

            return totalVisible.load(std::memory_order_relaxed);
        }

      private:
        Plane m_planes[kPlaneCount];
    };

} // namespace TestParallelCulling

using namespace TestParallelCulling;

TEST(ParallelCulling_SingleThreadedMatchesParallel)
{
    Frustum frustum;
    frustum.SetupBox(-10.0f, 10.0f, -10.0f, 10.0f, -10.0f, 10.0f);

    // Generate 1000 AABBs — some inside, some outside
    constexpr uint32_t count = 1000;
    std::vector<AABB> boxes(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        float x = static_cast<float>(i % 50) - 25.0f; // Range: [-25, 24]
        float y = static_cast<float>((i / 50) % 20) - 10.0f;
        float z = 0.0f;
        boxes[i] = {{x - 0.5f, y - 0.5f, z - 0.5f}, {x + 0.5f, y + 0.5f, z + 0.5f}};
    }

    auto singleResult = std::make_unique<bool[]>(count);
    auto parallelResult = std::make_unique<bool[]>(count);

    uint32_t singleVisible = frustum.BatchCull(boxes.data(), count, singleResult.get());
    uint32_t parallelVisible = frustum.ParallelBatchCull(boxes.data(), count, parallelResult.get(), 64);

    EXPECT_EQ(singleVisible, parallelVisible);

    // Verify every element matches
    for (uint32_t i = 0; i < count; ++i)
    {
        EXPECT_EQ(singleResult[i], parallelResult[i]);
    }
}

TEST(ParallelCulling_AllInsideFrustum)
{
    Frustum frustum;
    frustum.SetupBox(-100.0f, 100.0f, -100.0f, 100.0f, -100.0f, 100.0f);

    constexpr uint32_t count = 500;
    std::vector<AABB> boxes(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        float x = static_cast<float>(i % 10);
        boxes[i] = {{x, 0, 0}, {x + 1, 1, 1}};
    }

    auto result = std::make_unique<bool[]>(count);
    uint32_t visible = frustum.ParallelBatchCull(boxes.data(), count, result.get(), 64);

    EXPECT_EQ(visible, count);
    for (uint32_t i = 0; i < count; ++i)
    {
        EXPECT_TRUE(result[i]);
    }
}

TEST(ParallelCulling_AllOutsideFrustum)
{
    Frustum frustum;
    frustum.SetupBox(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);

    constexpr uint32_t count = 200;
    std::vector<AABB> boxes(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        float x = 50.0f + static_cast<float>(i); // All far outside
        boxes[i] = {{x, x, x}, {x + 1, x + 1, x + 1}};
    }

    auto result = std::make_unique<bool[]>(count);
    uint32_t visible = frustum.ParallelBatchCull(boxes.data(), count, result.get(), 64);

    EXPECT_EQ(visible, 0u);
    for (uint32_t i = 0; i < count; ++i)
    {
        EXPECT_FALSE(result[i]);
    }
}

TEST(ParallelCulling_SmallCountFallsBackToSingleThread)
{
    Frustum frustum;
    frustum.SetupBox(-10.0f, 10.0f, -10.0f, 10.0f, -10.0f, 10.0f);

    constexpr uint32_t count = 5;
    std::vector<AABB> boxes(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        boxes[i] = {{0, 0, 0}, {1, 1, 1}};
    }

    auto result = std::make_unique<bool[]>(count);
    // minBatchSize > count → should run single-threaded path
    uint32_t visible = frustum.ParallelBatchCull(boxes.data(), count, result.get(), 256);

    EXPECT_EQ(visible, count);
}

TEST(ParallelCulling_MixedVisibility)
{
    Frustum frustum;
    frustum.SetupBox(0.0f, 10.0f, 0.0f, 10.0f, 0.0f, 10.0f);

    // 4 boxes: 2 inside, 2 outside
    AABB boxes[4] = {
        {{1, 1, 1}, {2, 2, 2}},             // inside
        {{-50, -50, -50}, {-49, -49, -49}}, // outside
        {{5, 5, 5}, {6, 6, 6}},             // inside
        {{20, 20, 20}, {21, 21, 21}},       // outside
    };

    bool result[4] = {};
    uint32_t visible = frustum.ParallelBatchCull(boxes, 4, result, 1);

    EXPECT_EQ(visible, 2u);
    EXPECT_TRUE(result[0]);
    EXPECT_FALSE(result[1]);
    EXPECT_TRUE(result[2]);
    EXPECT_FALSE(result[3]);
}
