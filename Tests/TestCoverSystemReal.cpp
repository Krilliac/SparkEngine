/**
 * @file TestCoverSystemReal.cpp
 * @brief Real-class tests for Spark::AI::CoverSystem
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Engine/AI/CoverSystem.h"

namespace
{
    void ResetCoverSystem()
    {
        auto& cs = Spark::AI::CoverSystem::GetInstance();
        cs.Shutdown();
        cs.Initialize();
    }
} // namespace

TEST(CoverSystemReal_SingletonStable)
{
    auto& a = Spark::AI::CoverSystem::GetInstance();
    auto& b = Spark::AI::CoverSystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(CoverSystemReal_InitializeShutdown)
{
    auto& cs = Spark::AI::CoverSystem::GetInstance();
    cs.Shutdown();
    cs.Initialize();
    cs.Shutdown();
    cs.Initialize(); // Restore for subsequent tests.
}

TEST(CoverSystemReal_RegisterCoverPoint)
{
    ResetCoverSystem();
    auto& cs = Spark::AI::CoverSystem::GetInstance();

    Spark::AI::CoverPoint pt;
    pt.position = {10.0f, 0.0f, 20.0f};
    pt.normal = {1.0f, 0.0f, 0.0f};
    const uint32_t id = cs.RegisterCoverPoint(pt);
    EXPECT_TRUE(id > 0);
}

TEST(CoverSystemReal_FindNearestCoverEmpty)
{
    ResetCoverSystem();
    auto& cs = Spark::AI::CoverSystem::GetInstance();
    // No cover registered — should return nullptr.
    const auto* nearest = cs.FindNearestCover({0, 0, 0}, {1, 0, 0}, 100.0f);
    EXPECT_TRUE(nearest == nullptr);
}

TEST(CoverSystemReal_FindNearestCoverAfterRegister)
{
    ResetCoverSystem();
    auto& cs = Spark::AI::CoverSystem::GetInstance();

    Spark::AI::CoverPoint pt;
    pt.position = {10.0f, 0.0f, 0.0f};
    pt.normal = {-1.0f, 0.0f, 0.0f};
    cs.RegisterCoverPoint(pt);

    const auto* nearest = cs.FindNearestCover({0, 0, 0}, {1, 0, 0}, 100.0f);
    EXPECT_TRUE(nearest != nullptr);
}
