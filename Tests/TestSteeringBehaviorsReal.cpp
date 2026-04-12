/**
 * @file TestSteeringBehaviorsReal.cpp
 * @brief Real-class tests for Spark::AI::SteeringBehaviors namespace
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Engine/AI/SteeringBehaviors.h"

#include <cmath>

namespace
{
    bool VecNear(const XMFLOAT3& a, const XMFLOAT3& b, float eps = 0.01f)
    {
        return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
    }

    float VecLength(const XMFLOAT3& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
} // namespace

TEST(SteeringBehaviorsReal_SeekPointsTowardTarget)
{
    XMFLOAT3 pos{0, 0, 0};
    XMFLOAT3 target{10, 0, 0};
    auto velocity = Spark::AI::SteeringBehaviors::Seek(pos, target, 5.0f);
    // Velocity should point in +X direction at max speed.
    EXPECT_NEAR(velocity.x, 5.0f, 0.01f);
    EXPECT_NEAR(velocity.y, 0.0f, 0.01f);
    EXPECT_NEAR(velocity.z, 0.0f, 0.01f);
}

TEST(SteeringBehaviorsReal_SeekMagnitudeIsMaxSpeed)
{
    XMFLOAT3 pos{0, 0, 0};
    XMFLOAT3 target{100, 100, 0};
    const float maxSpeed = 7.5f;
    auto velocity = Spark::AI::SteeringBehaviors::Seek(pos, target, maxSpeed);
    EXPECT_NEAR(VecLength(velocity), maxSpeed, 0.01f);
}

TEST(SteeringBehaviorsReal_FleePointsAwayFromThreat)
{
    XMFLOAT3 pos{5, 0, 0};
    XMFLOAT3 threat{0, 0, 0};
    auto velocity = Spark::AI::SteeringBehaviors::Flee(pos, threat, 10.0f);
    // Agent at +5x, threat at origin → flee in +X direction.
    EXPECT_NEAR(velocity.x, 10.0f, 0.01f);
    EXPECT_NEAR(velocity.y, 0.0f, 0.01f);
}

TEST(SteeringBehaviorsReal_FleeOppositeOfSeek)
{
    XMFLOAT3 pos{0, 0, 0};
    XMFLOAT3 other{10, 5, 0};
    auto seek = Spark::AI::SteeringBehaviors::Seek(pos, other, 3.0f);
    auto flee = Spark::AI::SteeringBehaviors::Flee(pos, other, 3.0f);
    // Seek + Flee should roughly cancel.
    EXPECT_NEAR(seek.x + flee.x, 0.0f, 0.01f);
    EXPECT_NEAR(seek.y + flee.y, 0.0f, 0.01f);
    EXPECT_NEAR(seek.z + flee.z, 0.0f, 0.01f);
}

TEST(SteeringBehaviorsReal_ArriveAtTargetZeroSpeed)
{
    XMFLOAT3 pos{10, 0, 0};
    XMFLOAT3 target{10, 0, 0}; // Already at target.
    auto velocity = Spark::AI::SteeringBehaviors::Arrive(pos, target, 5.0f, 2.0f);
    // At the target, desired speed should be ~0.
    EXPECT_NEAR(VecLength(velocity), 0.0f, 0.5f);
}
