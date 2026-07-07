// TestWorldOriginSystem.cpp - Tests for floating-point origin rebasing

#include "TestFramework.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/World/WorldOriginSystem.h"

#include <entt/entt.hpp>
#include <limits>

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

TEST(WorldOrigin_CoordinateRoundTrip)
{
    // LocalToAbsolute and AbsoluteToLocal must be exact inverses for any
    // accumulated offset, in both directions.
    WorldOriginSystem system;
    entt::registry registry;
    system.ForceRebase(registry, DirectX::XMFLOAT3{1000.0f, -2000.0f, 3000.0f});
    system.ForceRebase(registry, DirectX::XMFLOAT3{250.0f, 250.0f, -125.0f});

    const DirectX::XMFLOAT3 samples[] = {
        {0.0f, 0.0f, 0.0f}, {12.5f, -7.25f, 99.0f}, {-500.0f, 640.0f, -1024.0f}};
    for (const auto& p : samples)
    {
        auto backToLocal = system.AbsoluteToLocal(system.LocalToAbsolute(p));
        EXPECT_NEAR(backToLocal.x, p.x, 0.001f);
        EXPECT_NEAR(backToLocal.y, p.y, 0.001f);
        EXPECT_NEAR(backToLocal.z, p.z, 0.001f);

        auto backToAbs = system.LocalToAbsolute(system.AbsoluteToLocal(p));
        EXPECT_NEAR(backToAbs.x, p.x, 0.001f);
        EXPECT_NEAR(backToAbs.y, p.y, 0.001f);
        EXPECT_NEAR(backToAbs.z, p.z, 0.001f);
    }
}

TEST(WorldOrigin_ForceRebaseAccumulates)
{
    // After N ForceRebase calls, the accumulated offset equals the
    // component-wise sum and totalRebases == N.
    WorldOriginSystem system;
    entt::registry registry;
    const DirectX::XMFLOAT3 steps[] = {
        {100.0f, 0.0f, 0.0f}, {0.0f, 50.0f, 0.0f}, {-30.0f, 0.0f, 70.0f}};
    DirectX::XMFLOAT3 expected{0.0f, 0.0f, 0.0f};
    uint32_t n = 0;
    for (const auto& s : steps)
    {
        system.ForceRebase(registry, s);
        expected.x += s.x;
        expected.y += s.y;
        expected.z += s.z;
        ++n;
    }
    auto offset = system.GetAccumulatedOffset();
    EXPECT_NEAR(offset.x, expected.x, 0.001f);
    EXPECT_NEAR(offset.y, expected.y, 0.001f);
    EXPECT_NEAR(offset.z, expected.z, 0.001f);
    EXPECT_EQ(system.GetStats().totalRebases, n);
}

TEST(WorldOrigin_RejectsNonFiniteAndZeroOffset)
{
    WorldOriginSystem system;
    entt::registry registry;
    // Establish a known baseline.
    system.ForceRebase(registry, DirectX::XMFLOAT3{10.0f, 20.0f, 30.0f});
    auto baseline = system.GetAccumulatedOffset();
    uint32_t baseRebases = system.GetStats().totalRebases;

    // Non-finite offsets are rejected: no accumulation, no rebase counted.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    system.ForceRebase(registry, DirectX::XMFLOAT3{nan, 0.0f, 0.0f});
    system.ForceRebase(registry, DirectX::XMFLOAT3{0.0f, inf, 0.0f});
    // A zero offset is skipped.
    system.ForceRebase(registry, DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});

    auto after = system.GetAccumulatedOffset();
    EXPECT_NEAR(after.x, baseline.x, 0.001f);
    EXPECT_NEAR(after.y, baseline.y, 0.001f);
    EXPECT_NEAR(after.z, baseline.z, 0.001f);
    EXPECT_EQ(system.GetStats().totalRebases, baseRebases);
}

TEST(WorldOrigin_ShiftsOnlyRootEntities)
{
    // ShiftAllTransforms (via ForceRebase) must move only root entities
    // (parent == entt::null); children inherit via the hierarchy.
    WorldOriginSystem system;
    entt::registry registry;

    auto root = registry.create();
    auto& rootT = registry.emplace<Transform>(root);
    rootT.position = {1000.0f, 500.0f, -250.0f};
    rootT.parent = entt::null;

    auto child = registry.create();
    auto& childT = registry.emplace<Transform>(child);
    childT.position = {5.0f, 6.0f, 7.0f};
    childT.parent = root;

    system.ForceRebase(registry, DirectX::XMFLOAT3{100.0f, 100.0f, 100.0f});

    // Root shifts by -offset.
    const auto& rootAfter = registry.get<Transform>(root);
    EXPECT_NEAR(rootAfter.position.x, 900.0f, 0.001f);
    EXPECT_NEAR(rootAfter.position.y, 400.0f, 0.001f);
    EXPECT_NEAR(rootAfter.position.z, -350.0f, 0.001f);

    // Child (has a parent) is left untouched.
    const auto& childAfter = registry.get<Transform>(child);
    EXPECT_NEAR(childAfter.position.x, 5.0f, 0.001f);
    EXPECT_NEAR(childAfter.position.y, 6.0f, 0.001f);
    EXPECT_NEAR(childAfter.position.z, 7.0f, 0.001f);
}
