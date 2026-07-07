/**
 * @file TestCoverSystemReal.cpp
 * @brief Real-class tests for Spark::AI::CoverSystem
 *
 * Phase JJ deep-wire.
 *
 * These tests mutate the CoverSystem::GetInstance() singleton, so they must be
 * order-independent to survive the harness's --shuffle mode. Rather than
 * scattering ResetCoverSystem() calls (and relying on one test leaving the
 * singleton initialized "for subsequent tests"), every test runs through a
 * fixture whose SetUp() resets the singleton to a known state first.
 */

#include "TestFramework.h"
#include "Engine/AI/CoverSystem.h"

// Fixture: reset the CoverSystem singleton to a clean, initialized state before
// each test so execution order (including --shuffle) never affects the result.
struct CoverSystemFixture
{
    void SetUp()
    {
        auto& cs = Spark::AI::CoverSystem::GetInstance();
        cs.Shutdown();
        cs.Initialize();
    }

    void TearDown() {}
};

TEST_F(CoverSystemFixture, SingletonStable)
{
    auto& a = Spark::AI::CoverSystem::GetInstance();
    auto& b = Spark::AI::CoverSystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST_F(CoverSystemFixture, InitializeShutdown)
{
    auto& cs = Spark::AI::CoverSystem::GetInstance();
    cs.Shutdown();
    cs.Initialize();
    cs.Shutdown();
    cs.Initialize();
}

TEST_F(CoverSystemFixture, RegisterCoverPoint)
{
    auto& cs = Spark::AI::CoverSystem::GetInstance();

    Spark::AI::CoverPoint pt;
    pt.position = {10.0f, 0.0f, 20.0f};
    pt.normal = {1.0f, 0.0f, 0.0f};
    const uint32_t id = cs.RegisterCoverPoint(pt);
    EXPECT_TRUE(id > 0);
}

TEST_F(CoverSystemFixture, FindNearestCoverEmpty)
{
    auto& cs = Spark::AI::CoverSystem::GetInstance();
    // No cover registered — should return nullptr.
    const auto* nearest = cs.FindNearestCover({0, 0, 0}, {1, 0, 0}, 100.0f);
    EXPECT_TRUE(nearest == nullptr);
}

TEST_F(CoverSystemFixture, FindNearestCoverAfterRegister)
{
    auto& cs = Spark::AI::CoverSystem::GetInstance();

    Spark::AI::CoverPoint pt;
    pt.position = {10.0f, 0.0f, 0.0f};
    pt.normal = {-1.0f, 0.0f, 0.0f};
    cs.RegisterCoverPoint(pt);

    const auto* nearest = cs.FindNearestCover({0, 0, 0}, {1, 0, 0}, 100.0f);
    EXPECT_TRUE(nearest != nullptr);
}
