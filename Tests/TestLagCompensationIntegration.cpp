/**
 * @file TestLagCompensationIntegration.cpp
 * @brief Tests for lag compensation integrated with hit detection
 *
 * Standalone implementations for CI testing (no networking/socket dependency).
 * Tests rewind, interpolation, and ray-AABB intersection against rewound hitboxes.
 */

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TestLagCompIntegration
{

    struct Float3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    Float3 Lerp(const Float3& a, const Float3& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    struct EntitySnapshot
    {
        uint32_t networkID = 0;
        Float3 position;
        Float3 boundsMin;
        Float3 boundsMax;
    };

    struct Snapshot
    {
        float timestamp = 0.0f;
        std::vector<EntitySnapshot> entities;
    };

    class TestLagCompensator
    {
      public:
        void RecordSnapshot(const Snapshot& snapshot)
        {
            m_history.push_back(snapshot);
            float cutoff = snapshot.timestamp - m_maxDuration;
            m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
                                           [cutoff](const Snapshot& s) { return s.timestamp < cutoff; }),
                            m_history.end());
        }

        bool RewindToTime(float targetTime, Snapshot& out) const
        {
            if (m_history.empty())
                return false;

            const Snapshot* before = nullptr;
            const Snapshot* after = nullptr;

            for (const auto& s : m_history)
            {
                if (s.timestamp <= targetTime)
                    before = &s;
                if (s.timestamp >= targetTime && !after)
                    after = &s;
            }

            if (!before && !after)
                return false;

            if (before && after && before != after)
            {
                float dt = after->timestamp - before->timestamp;
                float t = (dt > 0.0f) ? (targetTime - before->timestamp) / dt : 0.0f;
                t = std::max(0.0f, std::min(1.0f, t));

                out.timestamp = targetTime;
                out.entities.clear();
                for (const auto& eb : before->entities)
                {
                    for (const auto& ea : after->entities)
                    {
                        if (eb.networkID == ea.networkID)
                        {
                            EntitySnapshot interp;
                            interp.networkID = eb.networkID;
                            interp.position = Lerp(eb.position, ea.position, t);
                            interp.boundsMin = Lerp(eb.boundsMin, ea.boundsMin, t);
                            interp.boundsMax = Lerp(eb.boundsMax, ea.boundsMax, t);
                            out.entities.push_back(interp);
                            break;
                        }
                    }
                }
                return true;
            }
            else if (before)
            {
                out = *before;
                return true;
            }
            else if (after)
            {
                out = *after;
                return true;
            }
            return false;
        }

        void SetMaxDuration(float sec) { m_maxDuration = sec; }

      private:
        std::vector<Snapshot> m_history;
        float m_maxDuration = 1.0f;
    };

    struct HitResult
    {
        bool hit = false;
        uint32_t entityID = 0;
        Float3 hitPoint;
    };

    bool RayAABB(const Float3& origin, const Float3& dir, const Float3& bmin, const Float3& bmax, float maxDist,
                 float& outT)
    {
        float invX = (dir.x != 0.0f) ? (1.0f / dir.x) : 1e30f;
        float invY = (dir.y != 0.0f) ? (1.0f / dir.y) : 1e30f;
        float invZ = (dir.z != 0.0f) ? (1.0f / dir.z) : 1e30f;

        float t1 = (bmin.x - origin.x) * invX;
        float t2 = (bmax.x - origin.x) * invX;
        float tmin = std::min(t1, t2);
        float tmax = std::max(t1, t2);

        t1 = (bmin.y - origin.y) * invY;
        t2 = (bmax.y - origin.y) * invY;
        tmin = std::max(tmin, std::min(t1, t2));
        tmax = std::min(tmax, std::max(t1, t2));

        t1 = (bmin.z - origin.z) * invZ;
        t2 = (bmax.z - origin.z) * invZ;
        tmin = std::max(tmin, std::min(t1, t2));
        tmax = std::min(tmax, std::max(t1, t2));

        if (tmax >= tmin && tmax >= 0.0f && tmin < maxDist)
        {
            outT = (tmin >= 0.0f) ? tmin : tmax;
            return true;
        }
        return false;
    }

    HitResult ValidateHit(TestLagCompensator& comp, float clientTimestamp, float halfRTT, const Float3& rayOrigin,
                          const Float3& rayDir, float maxDist = 1000.0f)
    {
        HitResult result;
        Snapshot rewound;
        if (!comp.RewindToTime(clientTimestamp - halfRTT, rewound))
            return result;

        float closest = maxDist;
        for (const auto& ent : rewound.entities)
        {
            float t = 0.0f;
            if (RayAABB(rayOrigin, rayDir, ent.boundsMin, ent.boundsMax, closest, t))
            {
                closest = t;
                result.hit = true;
                result.entityID = ent.networkID;
                result.hitPoint = {rayOrigin.x + rayDir.x * t, rayOrigin.y + rayDir.y * t, rayOrigin.z + rayDir.z * t};
            }
        }
        return result;
    }

} // namespace TestLagCompIntegration

using namespace TestLagCompIntegration;

TEST(LagComp_RewindAndRaycast)
{
    TestLagCompensator comp;

    // Record 3 snapshots: entity moves along X axis
    Snapshot s1;
    s1.timestamp = 0.0f;
    s1.entities.push_back({1, {0, 0, 5}, {-0.5f, -1, 4.5f}, {0.5f, 1, 5.5f}});
    comp.RecordSnapshot(s1);

    Snapshot s2;
    s2.timestamp = 0.5f;
    s2.entities.push_back({1, {5, 0, 5}, {4.5f, -1, 4.5f}, {5.5f, 1, 5.5f}});
    comp.RecordSnapshot(s2);

    Snapshot s3;
    s3.timestamp = 1.0f;
    s3.entities.push_back({1, {10, 0, 5}, {9.5f, -1, 4.5f}, {10.5f, 1, 5.5f}});
    comp.RecordSnapshot(s3);

    // Shoot at X=5 (middle position), rewind to t=0.5
    auto hit = ValidateHit(comp, 0.5f, 0.0f, {5, 0, 0}, {0, 0, 1});
    EXPECT_TRUE(hit.hit);
    EXPECT_EQ(hit.entityID, 1u);

    // Shoot at X=0 (first position) — should miss at t=0.5
    auto miss = ValidateHit(comp, 0.5f, 0.0f, {0, 0, 0}, {0, 0, 1});
    EXPECT_FALSE(miss.hit);
}

TEST(LagComp_RewindBeyondHistory)
{
    TestLagCompensator comp;
    comp.SetMaxDuration(2.0f);

    Snapshot s1;
    s1.timestamp = 1.0f;
    s1.entities.push_back({1, {0, 0, 5}, {-0.5f, -1, 4.5f}, {0.5f, 1, 5.5f}});
    comp.RecordSnapshot(s1);

    Snapshot s2;
    s2.timestamp = 2.0f;
    s2.entities.push_back({1, {5, 0, 5}, {4.5f, -1, 4.5f}, {5.5f, 1, 5.5f}});
    comp.RecordSnapshot(s2);

    // Request rewind to t=0.0 — older than any snapshot, should fall back to oldest (t=1.0)
    Snapshot rewound;
    bool ok = comp.RewindToTime(0.0f, rewound);
    EXPECT_TRUE(ok);
    EXPECT_NEAR(rewound.entities[0].position.x, 0.0f, 0.01f);
}

TEST(LagComp_ConcurrentRewind)
{
    TestLagCompensator comp;

    Snapshot s1;
    s1.timestamp = 0.0f;
    s1.entities.push_back({1, {0, 0, 5}, {-0.5f, -1, 4.5f}, {0.5f, 1, 5.5f}});
    s1.entities.push_back({2, {10, 0, 5}, {9.5f, -1, 4.5f}, {10.5f, 1, 5.5f}});
    comp.RecordSnapshot(s1);

    Snapshot s2;
    s2.timestamp = 1.0f;
    s2.entities.push_back({1, {5, 0, 5}, {4.5f, -1, 4.5f}, {5.5f, 1, 5.5f}});
    s2.entities.push_back({2, {15, 0, 5}, {14.5f, -1, 4.5f}, {15.5f, 1, 5.5f}});
    comp.RecordSnapshot(s2);

    // Two "concurrent" rewinds to different times — each should be independent
    auto hit1 = ValidateHit(comp, 0.25f, 0.0f, {1.25f, 0, 0}, {0, 0, 1});
    auto hit2 = ValidateHit(comp, 0.75f, 0.0f, {3.75f, 0, 0}, {0, 0, 1});

    // At t=0.25: entity 1 should be at ~(1.25, 0, 5)
    EXPECT_TRUE(hit1.hit);
    EXPECT_EQ(hit1.entityID, 1u);

    // At t=0.75: entity 1 should be at ~(3.75, 0, 5)
    EXPECT_TRUE(hit2.hit);
    EXPECT_EQ(hit2.entityID, 1u);
}

TEST(LagComp_HitboxInterpolation)
{
    TestLagCompensator comp;

    Snapshot s1;
    s1.timestamp = 0.0f;
    s1.entities.push_back({1, {0, 0, 5}, {-1, -1, 4}, {1, 1, 6}});
    comp.RecordSnapshot(s1);

    Snapshot s2;
    s2.timestamp = 1.0f;
    s2.entities.push_back({1, {10, 0, 5}, {9, -1, 4}, {11, 1, 6}});
    comp.RecordSnapshot(s2);

    // Rewind to t=0.5 — entity should be interpolated to (5, 0, 5)
    Snapshot rewound;
    bool ok = comp.RewindToTime(0.5f, rewound);
    EXPECT_TRUE(ok);
    EXPECT_EQ(rewound.entities.size(), 1u);

    // Position should be interpolated, not snapped to either endpoint
    EXPECT_NEAR(rewound.entities[0].position.x, 5.0f, 0.01f);
    EXPECT_NEAR(rewound.entities[0].boundsMin.x, 4.0f, 0.01f);
    EXPECT_NEAR(rewound.entities[0].boundsMax.x, 6.0f, 0.01f);
}
