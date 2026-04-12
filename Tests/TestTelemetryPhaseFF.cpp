/**
 * @file TestTelemetryPhaseFF.cpp
 * @brief Phase FF Theme 3D tests for Spark::TelemetrySystem
 */

#include "TestFramework.h"
#include "Utils/Telemetry.h"

namespace
{
    void ResetTelemetry()
    {
        auto& t = Spark::TelemetrySystem::GetInstance();
        t.Shutdown();
        Spark::TelemetryConfig cfg;
        cfg.enabled = false;
        cfg.consentGiven = false;
        t.Initialize(cfg);
    }
} // namespace

TEST(TelemetryPhaseFF_SingletonStable)
{
    auto& a = Spark::TelemetrySystem::GetInstance();
    auto& b = Spark::TelemetrySystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(TelemetryPhaseFF_InitializeShutdown)
{
    auto& t = Spark::TelemetrySystem::GetInstance();
    t.Shutdown();
    EXPECT_FALSE(t.IsInitialized());

    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    t.Initialize(cfg);
    EXPECT_TRUE(t.IsInitialized());

    t.Shutdown();
    EXPECT_FALSE(t.IsInitialized());

    // Restore disabled state for other tests.
    ResetTelemetry();
}

TEST(TelemetryPhaseFF_DisabledByDefault)
{
    ResetTelemetry();
    auto& t = Spark::TelemetrySystem::GetInstance();
    // After ResetTelemetry (enabled=false, consent=false),
    // IsInitialized is true but internal state is disabled.
    EXPECT_TRUE(t.IsInitialized());
    EXPECT_FALSE(t.HasConsent());
}

TEST(TelemetryPhaseFF_ConsentRoundTrip)
{
    ResetTelemetry();
    auto& t = Spark::TelemetrySystem::GetInstance();
    t.SetConsent(true);
    EXPECT_TRUE(t.HasConsent());
    t.SetConsent(false);
    EXPECT_FALSE(t.HasConsent());
}

TEST(TelemetryPhaseFF_ConfigRoundTrip)
{
    auto& t = Spark::TelemetrySystem::GetInstance();
    t.Shutdown();

    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    cfg.maxQueueSize = 512;
    t.Initialize(cfg);

    const auto& retrieved = t.GetConfig();
    EXPECT_EQ(retrieved.enabled, true);
    EXPECT_EQ(retrieved.consentGiven, true);
    EXPECT_EQ(retrieved.maxQueueSize, static_cast<uint32_t>(512));

    ResetTelemetry();
}

TEST(TelemetryPhaseFF_ConsoleStatusReturnsString)
{
    ResetTelemetry();
    auto& t = Spark::TelemetrySystem::GetInstance();
    const std::string status = t.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
}

TEST(TelemetryPhaseFF_ShutdownIsIdempotent)
{
    auto& t = Spark::TelemetrySystem::GetInstance();
    t.Shutdown();
    t.Shutdown();
    EXPECT_FALSE(t.IsInitialized());
    ResetTelemetry();
}
