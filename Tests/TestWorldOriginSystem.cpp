// TestWorldOriginSystem.cpp - Tests for floating-point origin rebasing

#include "TestFramework.h"
#include "Engine/World/WorldOriginSystem.h"

using namespace Spark::World;

TEST(WorldOrigin_DefaultState)
{
    WorldOriginSystem system;
    EXPECT_TRUE(system.IsEnabled());
    EXPECT_GT(system.GetRebasingThreshold(), 0.0f);
    auto offset = system.GetAccumulatedOffset();
    EXPECT_NEAR(offset.x, 0.0f, 0.01f);
    EXPECT_NEAR(offset.y, 0.0f, 0.01f);
    EXPECT_NEAR(offset.z, 0.0f, 0.01f);
}

TEST(WorldOrigin_SetThreshold)
{
    WorldOriginSystem system;
    system.SetRebasingThreshold(5000.0f);
    EXPECT_NEAR(system.GetRebasingThreshold(), 5000.0f, 0.01f);
}

TEST(WorldOrigin_EnableDisable)
{
    WorldOriginSystem system;
    system.SetEnabled(false);
    EXPECT_FALSE(system.IsEnabled());
    system.SetEnabled(true);
    EXPECT_TRUE(system.IsEnabled());
}

TEST(WorldOrigin_LocalToAbsoluteIdentity)
{
    WorldOriginSystem system;
    DirectX::XMFLOAT3 local = {10.0f, 20.0f, 30.0f};
    auto absolute = system.LocalToAbsolute(local);
    EXPECT_NEAR(absolute.x, 10.0f, 0.01f);
    EXPECT_NEAR(absolute.y, 20.0f, 0.01f);
    EXPECT_NEAR(absolute.z, 30.0f, 0.01f);
}

TEST(WorldOrigin_AbsoluteToLocalIdentity)
{
    WorldOriginSystem system;
    DirectX::XMFLOAT3 absolute = {10.0f, 20.0f, 30.0f};
    auto local = system.AbsoluteToLocal(absolute);
    EXPECT_NEAR(local.x, 10.0f, 0.01f);
    EXPECT_NEAR(local.y, 20.0f, 0.01f);
    EXPECT_NEAR(local.z, 30.0f, 0.01f);
}

TEST(WorldOrigin_StatsInitiallyZero)
{
    WorldOriginSystem system;
    auto stats = system.GetStats();
    EXPECT_EQ(stats.totalRebases, static_cast<uint32_t>(0));
}

TEST(WorldOrigin_Reset)
{
    WorldOriginSystem system;
    system.Reset();
    auto offset = system.GetAccumulatedOffset();
    EXPECT_NEAR(offset.x, 0.0f, 0.01f);
    EXPECT_NEAR(offset.y, 0.0f, 0.01f);
    EXPECT_NEAR(offset.z, 0.0f, 0.01f);
}

TEST(WorldOrigin_RegisterCallbackDoesNotCrash)
{
    WorldOriginSystem system;
    bool called = false;
    system.RegisterRebaseCallback([&called](const DirectX::XMFLOAT3&) { called = true; });
    // Callback registered but not yet triggered
    EXPECT_FALSE(called);
}
