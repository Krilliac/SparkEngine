// TestSpringArm.cpp — tests for the Spark::Graphics::SpringArmState camera
// collision-avoidance helper. The class is an intentional stateless utility
// used by SpringArmComponent in the ECS; the update math is pure CPU and
// easy to unit-test directly.

#include "TestFramework.h"
#include "Graphics/SpringArm.h"

#include <cmath>
#include <limits>

using namespace Spark::Graphics;

namespace
{
    constexpr float FAR_AWAY = std::numeric_limits<float>::max();
}

TEST(SpringArmState_DefaultsAreSane)
{
    SpringArmState arm;
    EXPECT_NEAR(arm.targetLength, 5.0f, 1e-5f);
    EXPECT_NEAR(arm.minLength, 0.5f, 1e-5f);
    EXPECT_NEAR(arm.currentLength, 5.0f, 1e-5f);
    EXPECT_TRUE(arm.doCollisionTest);
    EXPECT_FALSE(arm.isColliding);
}

TEST(SpringArmState_NoCollisionKeepsTargetLength)
{
    SpringArmState arm;
    arm.targetLength = 10.0f;
    arm.currentLength = 10.0f;

    // Hit distance farther than target → no collision.
    for (int i = 0; i < 60; ++i)
        arm.Update(FAR_AWAY, 0.016f);

    EXPECT_FALSE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 10.0f, 1e-3f);
}

TEST(SpringArmState_CollisionShortensArmTowardsHit)
{
    SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.probeRadius = 0.2f;
    arm.currentLength = 5.0f;
    arm.smoothSpeed = 30.0f; // converge quickly for the test

    // Obstacle at 2.0 m — expected length is max(2.0 - 0.2, minLength) = 1.8.
    for (int i = 0; i < 120; ++i)
        arm.Update(2.0f, 0.016f);

    EXPECT_TRUE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 1.8f, 0.05f);
}

TEST(SpringArmState_MinLengthClamp)
{
    SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.minLength = 1.5f;
    arm.probeRadius = 0.2f;
    arm.currentLength = 5.0f;
    arm.smoothSpeed = 30.0f;

    // Obstacle very close — (0.5 - 0.2) = 0.3, but minLength is 1.5.
    for (int i = 0; i < 120; ++i)
        arm.Update(0.5f, 0.016f);

    EXPECT_TRUE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 1.5f, 1e-3f);
}

TEST(SpringArmState_CollisionCanBeDisabled)
{
    SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 5.0f;
    arm.doCollisionTest = false;

    // Even with an obstacle at 1m, collision test is disabled.
    for (int i = 0; i < 60; ++i)
        arm.Update(1.0f, 0.016f);

    EXPECT_FALSE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 5.0f, 1e-3f);
}

TEST(SpringArmState_RecoversWhenObstacleClears)
{
    SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.probeRadius = 0.2f;
    arm.currentLength = 5.0f;
    arm.smoothSpeed = 30.0f;

    // Hit then clear.
    for (int i = 0; i < 120; ++i)
        arm.Update(2.0f, 0.016f);
    EXPECT_TRUE(arm.isColliding);

    for (int i = 0; i < 120; ++i)
        arm.Update(FAR_AWAY, 0.016f);

    EXPECT_FALSE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 5.0f, 0.05f);
}
