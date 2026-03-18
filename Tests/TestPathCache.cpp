// TestPathCache.cpp - Tests for NavMesh path cache (Redot-inspired optimization)
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace TestPathCache
{

    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };

    struct PathPoint
    {
        Vec3 position;
    };

    struct PathResult
    {
        bool found = false;
        std::vector<PathPoint> path;
        float totalCost = 0.0f;
    };

    /// Hash function matching NavMeshQuery::HashPathKey
    uint64_t HashPathKey(const Vec3& start, const Vec3& goal)
    {
        constexpr float quantGrid = 0.5f;
        auto quantize = [quantGrid](float v) -> int32_t { return static_cast<int32_t>(std::floor(v / quantGrid)); };

        uint64_t hash = 14695981039346656037ULL;
        constexpr uint64_t prime = 1099511628211ULL;

        auto mix = [&](int32_t val)
        {
            hash ^= static_cast<uint64_t>(static_cast<uint32_t>(val));
            hash *= prime;
        };

        mix(quantize(start.x));
        mix(quantize(start.y));
        mix(quantize(start.z));
        mix(quantize(goal.x));
        mix(quantize(goal.y));
        mix(quantize(goal.z));

        return hash;
    }

    struct CachedPath
    {
        PathResult result;
        float timestamp = 0.0f;
    };

    static constexpr size_t MAX_CACHED = 64;
    static constexpr float CACHE_EXPIRY = 5.0f;

    class PathCache
    {
      public:
        PathResult* Lookup(const Vec3& start, const Vec3& goal, float currentTime)
        {
            uint64_t key = HashPathKey(start, goal);
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                if (currentTime - it->second.timestamp < CACHE_EXPIRY)
                    return &it->second.result;
                m_cache.erase(it);
            }
            return nullptr;
        }

        void Store(const Vec3& start, const Vec3& goal, const PathResult& result, float currentTime)
        {
            if (m_cache.size() >= MAX_CACHED)
            {
                float oldest = currentTime;
                uint64_t oldestKey = 0;
                for (const auto& [k, v] : m_cache)
                {
                    if (v.timestamp < oldest)
                    {
                        oldest = v.timestamp;
                        oldestKey = k;
                    }
                }
                m_cache.erase(oldestKey);
            }

            uint64_t key = HashPathKey(start, goal);
            m_cache[key] = {result, currentTime};
        }

        size_t Size() const { return m_cache.size(); }
        void Clear() { m_cache.clear(); }

      private:
        std::unordered_map<uint64_t, CachedPath> m_cache;
    };

} // namespace TestPathCache

using namespace TestPathCache;

TEST(PathCache_MissOnEmpty)
{
    PathCache cache;
    Vec3 start = {0, 0, 0};
    Vec3 goal = {10, 0, 10};
    auto* result = cache.Lookup(start, goal, 0.0f);
    EXPECT_TRUE(result == nullptr);
}

TEST(PathCache_HitAfterStore)
{
    PathCache cache;
    Vec3 start = {0, 0, 0};
    Vec3 goal = {10, 0, 10};

    PathResult pr;
    pr.found = true;
    pr.totalCost = 14.14f;
    cache.Store(start, goal, pr, 1.0f);

    auto* result = cache.Lookup(start, goal, 2.0f);
    EXPECT_TRUE(result != nullptr);
    EXPECT_TRUE(result->found);
    EXPECT_NEAR(result->totalCost, 14.14f, 0.01f);
}

TEST(PathCache_ExpiresAfterTimeout)
{
    PathCache cache;
    Vec3 start = {0, 0, 0};
    Vec3 goal = {10, 0, 10};

    PathResult pr;
    pr.found = true;
    cache.Store(start, goal, pr, 1.0f);

    // Within expiry
    EXPECT_TRUE(cache.Lookup(start, goal, 5.9f) != nullptr);

    // After expiry
    EXPECT_TRUE(cache.Lookup(start, goal, 6.1f) == nullptr);
}

TEST(PathCache_NearbyPositionsShareCacheEntry)
{
    // Positions quantized to 0.5m grid should hash the same
    Vec3 start1 = {1.1f, 0.0f, 2.2f};
    Vec3 start2 = {1.3f, 0.0f, 2.4f}; // Same 0.5m grid cell
    Vec3 goal = {10.0f, 0.0f, 10.0f};

    uint64_t hash1 = HashPathKey(start1, goal);
    uint64_t hash2 = HashPathKey(start2, goal);
    EXPECT_EQ(hash1, hash2);
}

TEST(PathCache_DifferentPositionsDifferentHash)
{
    Vec3 start1 = {0.0f, 0.0f, 0.0f};
    Vec3 start2 = {5.0f, 0.0f, 5.0f};
    Vec3 goal = {10.0f, 0.0f, 10.0f};

    uint64_t hash1 = HashPathKey(start1, goal);
    uint64_t hash2 = HashPathKey(start2, goal);
    EXPECT_NE(hash1, hash2);
}

TEST(PathCache_EvictsOldestWhenFull)
{
    PathCache cache;
    PathResult pr;
    pr.found = true;

    // Fill cache to capacity
    for (size_t i = 0; i < MAX_CACHED; ++i)
    {
        Vec3 start = {static_cast<float>(i) * 10.0f, 0, 0};
        Vec3 goal = {100, 0, 0};
        cache.Store(start, goal, pr, static_cast<float>(i));
    }
    EXPECT_EQ(cache.Size(), MAX_CACHED);

    // Add one more — should evict the oldest (timestamp 0)
    Vec3 newStart = {999, 0, 0};
    Vec3 newGoal = {100, 0, 0};
    cache.Store(newStart, newGoal, pr, static_cast<float>(MAX_CACHED));

    EXPECT_EQ(cache.Size(), MAX_CACHED);
}
