/**
 * @file TestLagCompensation.cpp
 * @brief Tests for Spark::Net::LagCompensation (engine hit-rewind ring buffer)
 *
 * Exercises the real engine class (unlike TestLagCompensationIntegration.cpp,
 * which tests a standalone mock): snapshot recording, exact-time and
 * interpolated rewinds, misses, ignoreEntity, nearest-hit selection, and
 * window eviction. No sockets — CI-safe.
 */

#include "TestFramework.h"

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/LagCompensation.h"

#include <limits>

using Spark::Net::LagCompensation;
using Spark::Net::RewindPose;

namespace
{
    // Standard test capsule: feet at (x, 0, z), 2m tall, 0.5m radius.
    RewindPose Pose(uint32_t id, float x, float z, float radius = 0.5f, float height = 2.0f)
    {
        RewindPose p;
        p.entityId = id;
        p.pos[0] = x;
        p.pos[1] = 0.0f;
        p.pos[2] = z;
        p.radius = radius;
        p.height = height;
        return p;
    }

    // Ray from (x, y, 0) firing straight down +Z.
    uint32_t CastZ(const LagCompensation& lc, double rewindTime, float x, float y, uint32_t ignore = 0,
                   float* outDist = nullptr, float* outHitPoint = nullptr)
    {
        const float origin[3] = {x, y, 0.0f};
        const float dir[3] = {0.0f, 0.0f, 1.0f};
        return lc.RewindRaycast(rewindTime, origin, dir, 1000.0f, ignore, outHitPoint, outDist);
    }
} // namespace

TEST(LagCompensation_EmptyHistoryMisses)
{
    LagCompensation lc;
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 0u);
}

TEST(LagCompensation_ExactTimeRewind)
{
    LagCompensation lc;
    lc.Configure(2.0f, 10.0f);

    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});
    lc.RecordSnapshot(1.0, {Pose(1, 10.0f, 5.0f)});

    // At t=0 the entity is at x=0; at t=1 it is at x=10.
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 1u);
    EXPECT_EQ(CastZ(lc, 1.0, 10.0f, 1.0f), 1u);

    // Shooting the OLD position at the NEW time misses (and vice versa).
    EXPECT_EQ(CastZ(lc, 1.0, 0.0f, 1.0f), 0u);
    EXPECT_EQ(CastZ(lc, 0.0, 10.0f, 1.0f), 0u);
}

TEST(LagCompensation_InterpolatedRewind)
{
    LagCompensation lc;
    lc.Configure(2.0f, 10.0f);

    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});
    lc.RecordSnapshot(1.0, {Pose(1, 10.0f, 5.0f)});

    // At t=0.5 the pose lerps to x=5.
    float dist = -1.0f;
    float hitPoint[3] = {0, 0, 0};
    EXPECT_EQ(CastZ(lc, 0.5, 5.0f, 1.0f, 0, &dist, hitPoint), 1u);
    // Capsule front face is at z = 5 - 0.5 = 4.5.
    EXPECT_NEAR(dist, 4.5f, 0.01f);
    EXPECT_NEAR(hitPoint[2], 4.5f, 0.01f);
    EXPECT_NEAR(hitPoint[0], 5.0f, 0.01f);

    // Neither endpoint position is hit at the midpoint time.
    EXPECT_EQ(CastZ(lc, 0.5, 0.0f, 1.0f), 0u);
    EXPECT_EQ(CastZ(lc, 0.5, 10.0f, 1.0f), 0u);
}

TEST(LagCompensation_MissLeavesOutputsUntouched)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});

    // Fire away from the entity.
    float dist = -123.0f;
    const float origin[3] = {0.0f, 1.0f, 0.0f};
    const float dir[3] = {0.0f, 0.0f, -1.0f};
    EXPECT_EQ(lc.RewindRaycast(0.0, origin, dir, 1000.0f, 0, nullptr, &dist), 0u);
    EXPECT_NEAR(dist, -123.0f, 0.001f); // only written on hit

    // Beyond maxDist is also a miss.
    const float fwd[3] = {0.0f, 0.0f, 1.0f};
    EXPECT_EQ(lc.RewindRaycast(0.0, origin, fwd, 2.0f, 0, nullptr, nullptr), 0u);
}

TEST(LagCompensation_NormalizesDirectionAndRejectsNonFiniteInput)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});

    const float origin[3] = {0.0f, 1.0f, 0.0f};
    const float scaledDirection[3] = {0.0f, 0.0f, 10.0f};
    float distance = -1.0f;
    float point[3] = {};
    EXPECT_EQ(lc.RewindRaycast(0.0, origin, scaledDirection, 10.0f, 0, point, &distance), 1u);
    EXPECT_NEAR(distance, 4.5f, 0.01f);
    EXPECT_NEAR(point[2], 4.5f, 0.01f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float badDirection[3] = {nan, 0.0f, 1.0f};
    EXPECT_EQ(lc.RewindRaycast(0.0, origin, badDirection, 10.0f, 0, nullptr, nullptr), 0u);
    const float overflowingDirection[3] = {(std::numeric_limits<float>::max)(),
                                           (std::numeric_limits<float>::max)(), 0.0f};
    EXPECT_EQ(lc.RewindRaycast(0.0, origin, overflowingDirection, 10.0f, 0, nullptr, nullptr), 0u);
    EXPECT_EQ(lc.RewindRaycast(std::numeric_limits<double>::infinity(), origin, scaledDirection, 10.0f, 0, nullptr,
                               nullptr),
              0u);
}

TEST(LagCompensation_NearestHitWins)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    // Two entities on the same ray, entity 2 in front of entity 3.
    lc.RecordSnapshot(0.0, {Pose(3, 0.0f, 20.0f), Pose(2, 0.0f, 5.0f)});

    float dist = 0.0f;
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f, 0, &dist), 2u);
    EXPECT_NEAR(dist, 4.5f, 0.01f);
}

TEST(LagCompensation_IgnoreEntity)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    lc.RecordSnapshot(0.0, {Pose(2, 0.0f, 5.0f), Pose(3, 0.0f, 20.0f)});

    // Ignoring the nearer entity exposes the farther one.
    float dist = 0.0f;
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f, /*ignore=*/2, &dist), 3u);
    EXPECT_NEAR(dist, 19.5f, 0.01f);

    // Ignoring the only remaining entity as well -> full miss is impossible
    // in one call, but ignoring the far one still returns the near one.
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f, /*ignore=*/3), 2u);
}

TEST(LagCompensation_EntityIdZeroIsReserved)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    // Poses with the reserved id 0 must never be hit.
    lc.RecordSnapshot(0.0, {Pose(0, 0.0f, 5.0f)});
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 0u);
}

TEST(LagCompensation_WindowEviction)
{
    LagCompensation lc;
    lc.Configure(0.5f, 10.0f); // 500 ms window at 10 Hz

    // Entity moves +1 in x per 100 ms for a full second.
    for (int i = 0; i <= 10; ++i)
        lc.RecordSnapshot(0.1 * i, {Pose(1, static_cast<float>(i), 5.0f)});

    // t=0 (entity at x=0) fell out of the 0.5 s window; the request clamps to
    // the oldest retained snapshot (t=0.5, x=5).
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 0u);
    EXPECT_EQ(CastZ(lc, 0.0, 5.0f, 1.0f), 1u);

    // Within the window, rewind still works normally.
    EXPECT_EQ(CastZ(lc, 0.8, 8.0f, 1.0f), 1u);

    // Future times clamp to the newest snapshot (x=10).
    EXPECT_EQ(CastZ(lc, 99.0, 10.0f, 1.0f), 1u);
}

TEST(LagCompensation_VerticalCapsuleShape)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    // Feet at y=0, height 2, radius 0.5 -> occupies y in [0, 2].
    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});

    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 1u);  // torso
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.8f), 1u);  // head cap
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 0.2f), 1u);  // shins cap
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 2.4f), 0u);  // above the head
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, -0.4f), 0u); // below the feet
    EXPECT_EQ(CastZ(lc, 0.0, 0.9f, 1.0f), 0u);  // wide of the capsule
}

TEST(LagCompensation_ClearDropsHistory)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 1u);

    lc.Clear();
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 0u);

    // Still usable after Clear().
    lc.RecordSnapshot(5.0, {Pose(1, 1.0f, 5.0f)});
    EXPECT_EQ(CastZ(lc, 5.0, 1.0f, 1.0f), 1u);
}

TEST(LagCompensation_ReconfigureResets)
{
    LagCompensation lc;
    lc.Configure(1.0f, 10.0f);
    lc.RecordSnapshot(0.0, {Pose(1, 0.0f, 5.0f)});

    lc.Configure(0.25f, 60.0f); // reconfiguring clears history
    EXPECT_EQ(CastZ(lc, 0.0, 0.0f, 1.0f), 0u);
}

#endif // ENABLE_NETWORKING
