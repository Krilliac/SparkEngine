/**
 * @file TestFaultIsolationReal.cpp
 * @brief Real-class tests for Spark::SubsystemFaultIsolator
 */

#include "TestFramework.h"
#include "Core/FaultIsolation.h"

namespace
{
    void ResetIsolator()
    {
        auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
        iso.ResetAll();
    }
} // namespace

TEST(FaultIsolationReal_SingletonStable)
{
    auto& a = Spark::SubsystemFaultIsolator::GetInstance();
    auto& b = Spark::SubsystemFaultIsolator::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(FaultIsolationReal_UnknownSubsystemIsEnabled)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    // Unknown subsystems are enabled by default (opt-in fault tracking).
    EXPECT_TRUE(iso.IsEnabled("PhaseKK_UnknownSubsystem"));
}

TEST(FaultIsolationReal_ReportFaultDoesNotCrash)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    iso.ReportFault("PhaseKK_FaultTest", "synthetic test error");
    // The fault should now be tracked. Default max retries is 3, so
    // a single fault shouldn't disable it yet.
    EXPECT_TRUE(iso.IsEnabled("PhaseKK_FaultTest"));
}

TEST(FaultIsolationReal_RepeatedFaultsDisable)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    iso.SetGlobalMaxRetries(2);
    // Report 3 faults — should exceed the retry limit and disable.
    iso.ReportFault("PhaseKK_RepeatedFault", "err1");
    iso.ReportFault("PhaseKK_RepeatedFault", "err2");
    iso.ReportFault("PhaseKK_RepeatedFault", "err3");
    EXPECT_FALSE(iso.IsEnabled("PhaseKK_RepeatedFault"));
    // Restore default for other tests.
    iso.SetGlobalMaxRetries(3);
    iso.ResetAll();
}

TEST(FaultIsolationReal_ResetSubsystemReenables)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    iso.SetGlobalMaxRetries(1);
    iso.ReportFault("PhaseKK_ResetTest", "err");
    iso.ReportFault("PhaseKK_ResetTest", "err");
    EXPECT_FALSE(iso.IsEnabled("PhaseKK_ResetTest"));

    iso.ResetSubsystem("PhaseKK_ResetTest");
    EXPECT_TRUE(iso.IsEnabled("PhaseKK_ResetTest"));

    iso.SetGlobalMaxRetries(3);
}

TEST(FaultIsolationReal_ResetAllReenables)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    iso.SetGlobalMaxRetries(1);
    iso.ReportFault("PhaseKK_A", "err");
    iso.ReportFault("PhaseKK_A", "err");
    iso.ReportFault("PhaseKK_B", "err");
    iso.ReportFault("PhaseKK_B", "err");

    iso.ResetAll();
    EXPECT_TRUE(iso.IsEnabled("PhaseKK_A"));
    EXPECT_TRUE(iso.IsEnabled("PhaseKK_B"));

    iso.SetGlobalMaxRetries(3);
}

TEST(FaultIsolationReal_GetRecordsSnapshot)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    iso.ReportFault("PhaseKK_SnapTest", "err");
    auto records = iso.GetRecordsSnapshot();
    EXPECT_TRUE(records.size() >= static_cast<size_t>(1));
    EXPECT_TRUE(records.find("PhaseKK_SnapTest") != records.end());
    iso.ResetAll();
}

TEST(FaultIsolationReal_UpdateIsSafeWithoutFaults)
{
    ResetIsolator();
    auto& iso = Spark::SubsystemFaultIsolator::GetInstance();
    iso.Update(1.0f);
    iso.Update(2.0f);
    EXPECT_TRUE(true);
}
