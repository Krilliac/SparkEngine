/**
 * @file TestSpringArmReal.cpp
 * @brief Real-class tests for Spark::Graphics::SpringArmState
 */

#include "TestFramework.h"
#include "Graphics/SpringArm.h"

#include <cfloat>

TEST(SpringArmReal_DefaultValues)
{
    Spark::Graphics::SpringArmState arm;
    EXPECT_NEAR(arm.targetLength, 5.0f, 0.01f);
    EXPECT_NEAR(arm.currentLength, 5.0f, 0.01f);
    EXPECT_NEAR(arm.probeRadius, 0.2f, 0.01f);
    EXPECT_NEAR(arm.minLength, 0.5f, 0.01f);
    EXPECT_TRUE(arm.doCollisionTest);
    EXPECT_FALSE(arm.isColliding);
}

TEST(SpringArmReal_NoCollisionMaintainsLength)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 5.0f;

    arm.Update(FLT_MAX, 0.016f);
    EXPECT_FALSE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 5.0f, 0.01f);
}

TEST(SpringArmReal_CollisionShortensArm)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 5.0f;
    arm.smoothSpeed = 100.0f; // fast smoothing for test

    arm.Update(3.0f, 0.1f);
    EXPECT_TRUE(arm.isColliding);
    EXPECT_LT(arm.currentLength, 5.0f);
}

TEST(SpringArmReal_MinLengthClamp)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 5.0f;
    arm.minLength = 1.0f;
    arm.smoothSpeed = 1000.0f;

    // Hit very close to target
    arm.Update(0.1f, 1.0f);
    EXPECT_GE(arm.currentLength, arm.minLength);
}

TEST(SpringArmReal_CollisionTestDisabled)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 5.0f;
    arm.doCollisionTest = false;

    arm.Update(1.0f, 0.016f);
    EXPECT_FALSE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 5.0f, 0.1f);
}

TEST(SpringArmReal_SmoothRecovery)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 2.0f; // start short
    arm.smoothSpeed = 10.0f;

    // No collision, should recover toward 5.0
    arm.Update(FLT_MAX, 0.1f);
    EXPECT_GT(arm.currentLength, 2.0f);
    EXPECT_LE(arm.currentLength, 5.0f);
}

TEST(SpringArmReal_ClampToTargetLength)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 5.0f;
    arm.currentLength = 6.0f; // over the target

    arm.Update(FLT_MAX, 0.1f);
    EXPECT_LE(arm.currentLength, arm.targetLength);
}

TEST(SpringArmReal_ProbeRadiusSubtracted)
{
    Spark::Graphics::SpringArmState arm;
    arm.targetLength = 10.0f;
    arm.currentLength = 10.0f;
    arm.probeRadius = 0.5f;
    arm.smoothSpeed = 1000.0f;
    arm.minLength = 0.1f;

    // Hit at 4.0 => desired = 4.0 - 0.5 = 3.5
    arm.Update(4.0f, 1.0f);
    EXPECT_TRUE(arm.isColliding);
    EXPECT_NEAR(arm.currentLength, 3.5f, 0.1f);
}
