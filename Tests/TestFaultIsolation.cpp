// TestFaultIsolation.cpp - Tests for runtime subsystem fault isolation

#include "TestFramework.h"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

// ============================================================================
// Standalone reimplementation of fault isolation types for testing
// (mirrors Spark::SubsystemFaultIsolator from FaultIsolation.h)
// ============================================================================

namespace
{

    struct FaultRecord
    {
        uint32_t faultCount = 0;
        uint32_t maxRetries = 0;
        bool enabled = true;
        std::string lastError;
        uint64_t lastFaultFrame = 0;
    };

    class TestFaultIsolator
    {
      public:
        void ReportFault(const std::string& name, const std::string& error, uint64_t frame = 0)
        {
            auto& record = m_records[name];
            record.faultCount++;
            record.lastError = error;
            record.lastFaultFrame = frame;

            uint32_t maxRetries = record.maxRetries > 0 ? record.maxRetries : m_globalMaxRetries;
            if (record.faultCount >= maxRetries)
            {
                record.enabled = false;
            }
        }

        bool IsEnabled(const std::string& name) const
        {
            auto it = m_records.find(name);
            if (it == m_records.end())
                return true;
            return it->second.enabled;
        }

        void ResetSubsystem(const std::string& name)
        {
            auto it = m_records.find(name);
            if (it == m_records.end())
                return;
            it->second.faultCount = 0;
            it->second.enabled = true;
            it->second.lastError.clear();
        }

        void ResetAll()
        {
            for (auto& [name, record] : m_records)
            {
                record.faultCount = 0;
                record.enabled = true;
                record.lastError.clear();
            }
        }

        void SetMaxRetries(const std::string& name, uint32_t maxRetries) { m_records[name].maxRetries = maxRetries; }

        void SetGlobalMaxRetries(uint32_t maxRetries) { m_globalMaxRetries = maxRetries; }

        uint32_t GetEffectiveMaxRetries(const std::string& name) const
        {
            auto it = m_records.find(name);
            if (it != m_records.end() && it->second.maxRetries > 0)
                return it->second.maxRetries;
            return m_globalMaxRetries;
        }

        const FaultRecord* GetRecord(const std::string& name) const
        {
            auto it = m_records.find(name);
            return it != m_records.end() ? &it->second : nullptr;
        }

        size_t GetRecordCount() const { return m_records.size(); }

      private:
        std::unordered_map<std::string, FaultRecord> m_records;
        uint32_t m_globalMaxRetries = 3;
    };

} // anonymous namespace

// ============================================================================
// Tests: Basic fault tracking
// ============================================================================

TEST(FaultIsolation_UnknownSubsystemIsEnabled)
{
    TestFaultIsolator isolator;
    EXPECT_TRUE(isolator.IsEnabled("Weather"));
    EXPECT_TRUE(isolator.IsEnabled("Physics"));
    EXPECT_TRUE(isolator.IsEnabled("NonExistent"));
}

TEST(FaultIsolation_SingleFaultDoesNotDisable)
{
    TestFaultIsolator isolator;
    isolator.ReportFault("Weather", "test error");
    EXPECT_TRUE(isolator.IsEnabled("Weather"));

    auto* record = isolator.GetRecord("Weather");
    ASSERT_TRUE(record != nullptr);
    EXPECT_EQ(record->faultCount, 1u);
    EXPECT_EQ(record->lastError, std::string("test error"));
}

TEST(FaultIsolation_DisableAfterMaxRetries)
{
    TestFaultIsolator isolator;
    // Default global max retries is 3
    isolator.ReportFault("Weather", "error 1");
    EXPECT_TRUE(isolator.IsEnabled("Weather"));

    isolator.ReportFault("Weather", "error 2");
    EXPECT_TRUE(isolator.IsEnabled("Weather"));

    isolator.ReportFault("Weather", "error 3");
    EXPECT_FALSE(isolator.IsEnabled("Weather"));

    auto* record = isolator.GetRecord("Weather");
    EXPECT_EQ(record->faultCount, 3u);
    EXPECT_EQ(record->lastError, std::string("error 3"));
}

TEST(FaultIsolation_DisabledSubsystemStaysDisabled)
{
    TestFaultIsolator isolator;
    isolator.ReportFault("AI", "fault 1");
    isolator.ReportFault("AI", "fault 2");
    isolator.ReportFault("AI", "fault 3");
    EXPECT_FALSE(isolator.IsEnabled("AI"));

    // Additional faults don't change anything
    isolator.ReportFault("AI", "fault 4");
    EXPECT_FALSE(isolator.IsEnabled("AI"));
    EXPECT_EQ(isolator.GetRecord("AI")->faultCount, 4u);
}

// ============================================================================
// Tests: Reset
// ============================================================================

TEST(FaultIsolation_ResetSubsystem)
{
    TestFaultIsolator isolator;
    isolator.ReportFault("Audio", "crash");
    isolator.ReportFault("Audio", "crash");
    isolator.ReportFault("Audio", "crash");
    EXPECT_FALSE(isolator.IsEnabled("Audio"));

    isolator.ResetSubsystem("Audio");
    EXPECT_TRUE(isolator.IsEnabled("Audio"));

    auto* record = isolator.GetRecord("Audio");
    EXPECT_EQ(record->faultCount, 0u);
    EXPECT_TRUE(record->lastError.empty());
}

TEST(FaultIsolation_ResetAll)
{
    TestFaultIsolator isolator;
    // Fault multiple subsystems
    for (int i = 0; i < 3; i++)
    {
        isolator.ReportFault("A", "fault");
        isolator.ReportFault("B", "fault");
    }
    EXPECT_FALSE(isolator.IsEnabled("A"));
    EXPECT_FALSE(isolator.IsEnabled("B"));

    isolator.ResetAll();
    EXPECT_TRUE(isolator.IsEnabled("A"));
    EXPECT_TRUE(isolator.IsEnabled("B"));
}

TEST(FaultIsolation_ResetNonexistentSubsystemIsNoOp)
{
    TestFaultIsolator isolator;
    isolator.ResetSubsystem("DoesNotExist"); // Should not crash
    EXPECT_TRUE(isolator.IsEnabled("DoesNotExist"));
}

// ============================================================================
// Tests: Configuration
// ============================================================================

TEST(FaultIsolation_GlobalMaxRetries)
{
    TestFaultIsolator isolator;
    isolator.SetGlobalMaxRetries(5);

    for (int i = 0; i < 4; i++)
    {
        isolator.ReportFault("Physics", "error");
        EXPECT_TRUE(isolator.IsEnabled("Physics"));
    }

    isolator.ReportFault("Physics", "error");
    EXPECT_FALSE(isolator.IsEnabled("Physics"));
}

TEST(FaultIsolation_PerSubsystemMaxRetries)
{
    TestFaultIsolator isolator;
    // Global is 3, but set Weather to 1
    isolator.SetMaxRetries("Weather", 1);

    isolator.ReportFault("Weather", "crash");
    EXPECT_FALSE(isolator.IsEnabled("Weather"));

    // Other subsystems still use global (3)
    isolator.ReportFault("Audio", "error");
    EXPECT_TRUE(isolator.IsEnabled("Audio"));
}

TEST(FaultIsolation_EffectiveMaxRetries)
{
    TestFaultIsolator isolator;
    EXPECT_EQ(isolator.GetEffectiveMaxRetries("Any"), 3u);

    isolator.SetMaxRetries("Custom", 10);
    EXPECT_EQ(isolator.GetEffectiveMaxRetries("Custom"), 10u);

    isolator.SetGlobalMaxRetries(7);
    EXPECT_EQ(isolator.GetEffectiveMaxRetries("Other"), 7u);
    EXPECT_EQ(isolator.GetEffectiveMaxRetries("Custom"), 10u);
}

// ============================================================================
// Tests: Isolation between subsystems
// ============================================================================

TEST(FaultIsolation_IndependentSubsystems)
{
    TestFaultIsolator isolator;

    // Fault Weather but not Physics
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Weather", "error");

    EXPECT_FALSE(isolator.IsEnabled("Weather"));
    EXPECT_TRUE(isolator.IsEnabled("Physics"));
    EXPECT_TRUE(isolator.IsEnabled("Audio"));
}

TEST(FaultIsolation_FrameNumberTracking)
{
    TestFaultIsolator isolator;
    isolator.ReportFault("AI", "error at frame 100", 100);
    isolator.ReportFault("AI", "error at frame 200", 200);

    auto* record = isolator.GetRecord("AI");
    EXPECT_EQ(record->lastFaultFrame, static_cast<uint64_t>(200));
}

// ============================================================================
// Tests: Guard macro simulation
// ============================================================================

TEST(FaultIsolation_GuardMacroSimulation)
{
    TestFaultIsolator isolator;
    int updateCount = 0;

    // Simulate SPARK_GUARDED_UPDATE behavior
    auto guardedUpdate = [&](const std::string& name, auto&& block)
    {
        if (!isolator.IsEnabled(name))
            return;
        try
        {
            block();
        }
        catch (const std::exception& ex)
        {
            isolator.ReportFault(name, ex.what());
        }
    };

    // First 3 calls: subsystem throws, gets faulted
    for (int i = 0; i < 3; i++)
    {
        guardedUpdate("Buggy", []() { throw std::runtime_error("crash"); });
    }
    EXPECT_FALSE(isolator.IsEnabled("Buggy"));

    // Subsequent calls: subsystem is skipped
    guardedUpdate("Buggy", [&]() { updateCount++; });
    EXPECT_EQ(updateCount, 0);

    // Reset and verify it runs again
    isolator.ResetSubsystem("Buggy");
    guardedUpdate("Buggy", [&]() { updateCount++; });
    EXPECT_EQ(updateCount, 1);
}

TEST(FaultIsolation_HealthySubsystemNotAffected)
{
    TestFaultIsolator isolator;
    int healthyCount = 0;
    int buggyCount = 0;

    auto guardedUpdate = [&](const std::string& name, auto&& block)
    {
        if (!isolator.IsEnabled(name))
            return;
        try
        {
            block();
        }
        catch (const std::exception& ex)
        {
            isolator.ReportFault(name, ex.what());
        }
    };

    // Simulate 5 frames
    for (int frame = 0; frame < 5; frame++)
    {
        guardedUpdate("Healthy", [&]() { healthyCount++; });
        guardedUpdate("Buggy",
                      [&]()
                      {
                          buggyCount++;
                          throw std::runtime_error("crash");
                      });
    }

    // Healthy ran all 5 frames
    EXPECT_EQ(healthyCount, 5);
    // Buggy ran 3 times (faulted on 3rd, skipped frame 4 and 5)
    EXPECT_EQ(buggyCount, 3);
    EXPECT_TRUE(isolator.IsEnabled("Healthy"));
    EXPECT_FALSE(isolator.IsEnabled("Buggy"));
}
