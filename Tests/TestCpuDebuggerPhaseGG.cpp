/**
 * @file TestCpuDebuggerPhaseGG.cpp
 * @brief Phase GG Theme 3D tests for Spark::CpuDebugger
 */

#include "TestFramework.h"
#include "Utils/CpuDebugger.h"

#include <chrono>
#include <thread>

namespace
{
    void ResetDebugger()
    {
        Spark::CpuDebugger::GetInstance().Reset();
        Spark::CpuDebugger::GetInstance().SetEnabled(true);
    }
} // namespace

TEST(CpuDebuggerPhaseGG_SingletonStable)
{
    auto& a = Spark::CpuDebugger::GetInstance();
    auto& b = Spark::CpuDebugger::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(CpuDebuggerPhaseGG_EnabledByDefault)
{
    ResetDebugger();
    EXPECT_TRUE(Spark::CpuDebugger::GetInstance().IsEnabled());
}

TEST(CpuDebuggerPhaseGG_SetEnabledToggle)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.SetEnabled(false);
    EXPECT_FALSE(cd.IsEnabled());
    cd.SetEnabled(true);
    EXPECT_TRUE(cd.IsEnabled());
}

TEST(CpuDebuggerPhaseGG_BeginEndSectionRecords)
{
    ResetDebugger();
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.BeginSection("PhaseGG_Section", "Test");
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    cd.EndSection("PhaseGG_Section");

    auto stats = cd.GetSectionStats("PhaseGG_Section");
    EXPECT_TRUE(stats.hitCount >= 1);
}

TEST(CpuDebuggerPhaseGG_MultipleCallsAggregate)
{
    ResetDebugger();
    auto& cd = Spark::CpuDebugger::GetInstance();
    for (int i = 0; i < 5; ++i)
    {
        cd.BeginSection("PhaseGG_Multi", "Test");
        cd.EndSection("PhaseGG_Multi");
    }
    auto stats = cd.GetSectionStats("PhaseGG_Multi");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(5));
}

TEST(CpuDebuggerPhaseGG_EndSectionWithoutBeginIsSafe)
{
    ResetDebugger();
    auto& cd = Spark::CpuDebugger::GetInstance();
    // EndSection with no matching Begin must not crash.
    cd.EndSection("PhaseGG_NoBegin");
    EXPECT_TRUE(cd.IsEnabled());
}

TEST(CpuDebuggerPhaseGG_ResetClearsAllSections)
{
    ResetDebugger();
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.BeginSection("PhaseGG_ToClear", "Test");
    cd.EndSection("PhaseGG_ToClear");

    cd.Reset();
    auto stats = cd.GetSectionStats("PhaseGG_ToClear");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(0));
}

TEST(CpuDebuggerPhaseGG_RecordWhenDisabledIsNoOp)
{
    ResetDebugger();
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.SetEnabled(false);
    cd.BeginSection("PhaseGG_Disabled", "Test");
    cd.EndSection("PhaseGG_Disabled");
    cd.SetEnabled(true);

    auto stats = cd.GetSectionStats("PhaseGG_Disabled");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(0));
}
