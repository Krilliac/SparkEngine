// TestTacticalPointSystem.cpp - Tests for tactical point registration, queries, and occupancy
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace TestTPS
{

    using PointID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        float DistanceTo(const Vec3& other) const
        {
            float dx = x - other.x, dy = y - other.y, dz = z - other.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    };

    enum class TacticalPointType : uint8_t
    {
        Sniper,
        Ambush,
        Patrol,
        Defend,
        Retreat
    };

    struct TacticalPoint
    {
        PointID id = 0;
        Vec3 position;
        TacticalPointType type = TacticalPointType::Patrol;
        float score = 1.0f;
        bool occupied = false;
    };

    struct TacticalPointSystem
    {
        std::vector<TacticalPoint> points;
        PointID nextID = 1;

        PointID RegisterPoint(Vec3 pos, TacticalPointType type, float score)
        {
            PointID id = nextID++;
            points.push_back({id, pos, type, score, false});
            return id;
        }

        const TacticalPoint* FindBestPoint(TacticalPointType type) const
        {
            const TacticalPoint* best = nullptr;
            for (const auto& p : points)
            {
                if (p.type == type && !p.occupied)
                {
                    if (!best || p.score > best->score)
                        best = &p;
                }
            }
            return best;
        }

        std::vector<const TacticalPoint*> FindPoints(TacticalPointType type, float maxDist, Vec3 origin) const
        {
            std::vector<const TacticalPoint*> result;
            for (const auto& p : points)
            {
                if (p.type == type && !p.occupied && origin.DistanceTo(p.position) <= maxDist)
                    result.push_back(&p);
            }
            return result;
        }

        bool MarkOccupied(PointID id)
        {
            for (auto& p : points)
                if (p.id == id)
                {
                    p.occupied = true;
                    return true;
                }
            return false;
        }

        bool MarkFree(PointID id)
        {
            for (auto& p : points)
                if (p.id == id)
                {
                    p.occupied = false;
                    return true;
                }
            return false;
        }
    };

} // namespace TestTPS

// ============================================================================
// Registration
// ============================================================================

TEST(TacticalPoint_RegisterPoint)
{
    TestTPS::TacticalPointSystem sys;
    auto id1 = sys.RegisterPoint({10.0f, 0.0f, 5.0f}, TestTPS::TacticalPointType::Sniper, 8.0f);
    auto id2 = sys.RegisterPoint({20.0f, 0.0f, 15.0f}, TestTPS::TacticalPointType::Ambush, 6.0f);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(static_cast<int>(sys.points.size()), 2);
}

// ============================================================================
// Queries
// ============================================================================

TEST(TacticalPoint_FindBestPoint)
{
    TestTPS::TacticalPointSystem sys;
    sys.RegisterPoint({0, 0, 0}, TestTPS::TacticalPointType::Sniper, 5.0f);
    sys.RegisterPoint({10, 0, 0}, TestTPS::TacticalPointType::Sniper, 9.0f);
    sys.RegisterPoint({20, 0, 0}, TestTPS::TacticalPointType::Ambush, 10.0f);

    const auto* best = sys.FindBestPoint(TestTPS::TacticalPointType::Sniper);
    ASSERT_TRUE(best != nullptr);
    EXPECT_NEAR(best->score, 9.0f, 0.001f);

    // Ambush type should return the ambush point
    const auto* bestAmbush = sys.FindBestPoint(TestTPS::TacticalPointType::Ambush);
    ASSERT_TRUE(bestAmbush != nullptr);
    EXPECT_NEAR(bestAmbush->score, 10.0f, 0.001f);
}

TEST(TacticalPoint_FindPoints_WithRadius)
{
    TestTPS::TacticalPointSystem sys;
    sys.RegisterPoint({0, 0, 0}, TestTPS::TacticalPointType::Patrol, 3.0f);
    sys.RegisterPoint({5, 0, 0}, TestTPS::TacticalPointType::Patrol, 4.0f);
    sys.RegisterPoint({100, 0, 0}, TestTPS::TacticalPointType::Patrol, 5.0f);

    auto results = sys.FindPoints(TestTPS::TacticalPointType::Patrol, 10.0f, {0, 0, 0});
    EXPECT_EQ(static_cast<int>(results.size()), 2);
}

// ============================================================================
// Occupancy
// ============================================================================

TEST(TacticalPoint_MarkOccupiedAndFree)
{
    TestTPS::TacticalPointSystem sys;
    auto id = sys.RegisterPoint({0, 0, 0}, TestTPS::TacticalPointType::Defend, 7.0f);

    EXPECT_TRUE(sys.MarkOccupied(id));
    // Occupied point should not appear in FindBestPoint
    EXPECT_TRUE(sys.FindBestPoint(TestTPS::TacticalPointType::Defend) == nullptr);

    EXPECT_TRUE(sys.MarkFree(id));
    EXPECT_TRUE(sys.FindBestPoint(TestTPS::TacticalPointType::Defend) != nullptr);

    // Invalid ID
    EXPECT_FALSE(sys.MarkOccupied(999));
}
