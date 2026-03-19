// TestCoverSystem.cpp - Tests for cover point registration, queries, and occupancy
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TestCover
{

    using CoverID = uint32_t;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        float DistanceTo(const Vec3& other) const
        {
            float dx = x - other.x, dy = y - other.y, dz = z - other.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        Vec3 Normalized() const
        {
            float len = std::sqrt(x * x + y * y + z * z);
            if (len < 0.0001f)
                return {0, 0, 0};
            return {x / len, y / len, z / len};
        }

        float Dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }

        Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    };

    enum class CoverHeight : uint8_t
    {
        Low,
        High
    };

    struct CoverPoint
    {
        CoverID id = 0;
        Vec3 position;
        Vec3 normal; // Direction the cover faces (away from wall)
        CoverHeight height = CoverHeight::High;
        bool occupied = false;
    };

    struct CoverSystem
    {
        std::vector<CoverPoint> coverPoints;
        CoverID nextID = 1;

        CoverID RegisterCoverPoint(Vec3 pos, Vec3 normal, CoverHeight height)
        {
            CoverID id = nextID++;
            coverPoints.push_back({id, pos, normal, height, false});
            return id;
        }

        const CoverPoint* FindNearestCover(Vec3 from) const
        {
            const CoverPoint* nearest = nullptr;
            float bestDist = 1e30f;
            for (const auto& cp : coverPoints)
            {
                if (cp.occupied)
                    continue;
                float d = from.DistanceTo(cp.position);
                if (d < bestDist)
                {
                    bestDist = d;
                    nearest = &cp;
                }
            }
            return nearest;
        }

        const CoverPoint* FindCoverFromThreat(Vec3 searchOrigin, Vec3 threatPos, float maxDist) const
        {
            const CoverPoint* best = nullptr;
            float bestScore = -1e30f;
            for (const auto& cp : coverPoints)
            {
                if (cp.occupied)
                    continue;
                float d = searchOrigin.DistanceTo(cp.position);
                if (d > maxDist)
                    continue;
                // Score: prefer cover whose normal faces away from threat
                Vec3 threatDir = (threatPos - cp.position).Normalized();
                float coverQuality = -threatDir.Dot(cp.normal); // Higher = better shielding
                float score = coverQuality - d * 0.01f;         // Slight distance penalty
                if (score > bestScore)
                {
                    bestScore = score;
                    best = &cp;
                }
            }
            return best;
        }

        bool MarkOccupied(CoverID id)
        {
            for (auto& cp : coverPoints)
                if (cp.id == id)
                {
                    cp.occupied = true;
                    return true;
                }
            return false;
        }

        bool MarkFree(CoverID id)
        {
            for (auto& cp : coverPoints)
                if (cp.id == id)
                {
                    cp.occupied = false;
                    return true;
                }
            return false;
        }
    };

} // namespace TestCover

// ============================================================================
// Registration
// ============================================================================

TEST(CoverSystem_RegisterCoverPoint)
{
    TestCover::CoverSystem sys;
    auto id1 = sys.RegisterCoverPoint({5, 0, 5}, {0, 0, 1}, TestCover::CoverHeight::High);
    auto id2 = sys.RegisterCoverPoint({10, 0, 10}, {1, 0, 0}, TestCover::CoverHeight::Low);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(static_cast<int>(sys.coverPoints.size()), 2);
}

// ============================================================================
// Queries
// ============================================================================

TEST(CoverSystem_FindNearestCover)
{
    TestCover::CoverSystem sys;
    sys.RegisterCoverPoint({10, 0, 0}, {0, 0, 1}, TestCover::CoverHeight::High);
    sys.RegisterCoverPoint({3, 0, 0}, {0, 0, 1}, TestCover::CoverHeight::Low);

    const auto* nearest = sys.FindNearestCover({0, 0, 0});
    EXPECT_TRUE(nearest != nullptr);
    EXPECT_NEAR(nearest->position.x, 3.0f, 0.001f);
}

TEST(CoverSystem_FindCoverFromThreat)
{
    TestCover::CoverSystem sys;
    // Cover facing away from threat (good)
    sys.RegisterCoverPoint({5, 0, 0}, {-1, 0, 0}, TestCover::CoverHeight::High);
    // Cover facing toward threat (bad)
    sys.RegisterCoverPoint({5, 0, 5}, {1, 0, 0}, TestCover::CoverHeight::High);

    // Threat at x=20, search from origin
    const auto* best = sys.FindCoverFromThreat({0, 0, 0}, {20, 0, 0}, 50.0f);
    EXPECT_TRUE(best != nullptr);
    // The cover with normal facing away from threat should score higher
    EXPECT_NEAR(best->normal.x, -1.0f, 0.001f);
}

// ============================================================================
// Occupancy
// ============================================================================

TEST(CoverSystem_MarkOccupiedAndFree)
{
    TestCover::CoverSystem sys;
    auto id = sys.RegisterCoverPoint({5, 0, 0}, {0, 0, 1}, TestCover::CoverHeight::High);

    EXPECT_TRUE(sys.MarkOccupied(id));
    EXPECT_TRUE(sys.FindNearestCover({0, 0, 0}) == nullptr);

    EXPECT_TRUE(sys.MarkFree(id));
    EXPECT_TRUE(sys.FindNearestCover({0, 0, 0}) != nullptr);

    EXPECT_FALSE(sys.MarkOccupied(999));
}
