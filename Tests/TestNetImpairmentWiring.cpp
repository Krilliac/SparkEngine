/**
 * @file TestNetImpairmentWiring.cpp
 * @brief TF-110: the real net_* console commands reach the InstabilitySimulator
 *        that NetworkManager consults, through the EngineSettings bridge.
 *
 * Before TF-110 net_lag/net_loss/net_jitter only wrote EngineSettings and
 * nothing read those fields, so the commands were no-ops. These tests run the
 * production commands (Spark::RegisterSubsystemConsoleCommands, the
 * TestEcsCameraConsole.cpp pattern), not a mirror. EngineSettings and the
 * simulator are process singletons, so every test restores both on exit.
 */
#include "TestFramework.h"

#include "Core/EngineSettings.h"
#include "Core/SubsystemConsoleCommands.h"
#include "Engine/Networking/InstabilitySimulator.h"
#include "Utils/SparkConsole.h"

#include <cstdint>
#include <limits>
#include <vector>

using Spark::Net::InstabilitySettings;
using Spark::Net::InstabilitySimulator;

namespace
{
    void ZeroImpairmentSettings()
    {
        auto& settings = EngineSettings::GetInstance();
        for (const char* key : {"SimulatedLatencyMs", "SimulatedPacketLoss", "SimulatedJitterMs",
                                "SimulatedReorderPercent", "SimulatedDuplicatePercent", "SimulatedImpairmentSeed"})
            settings.SetValue("Network", key, "0");
    }

    struct ImpairmentScope
    {
        ImpairmentScope()
        {
            ZeroImpairmentSettings();
            InstabilitySimulator::GetInstance().Shutdown();
            auto& console = Spark::SimpleConsole::GetInstance();
            console.Initialize();
            Spark::RegisterSubsystemConsoleCommands();
        }
        ~ImpairmentScope()
        {
            ZeroImpairmentSettings();
            InstabilitySimulator::GetInstance().Shutdown();
        }
        ImpairmentScope(const ImpairmentScope&) = delete;
        ImpairmentScope& operator=(const ImpairmentScope&) = delete;
    };

    bool Run(const char* line)
    {
        return Spark::SimpleConsole::GetInstance().ExecuteCommand(line);
    }
} // namespace

TEST(NetImpairment_ConsoleNetLagReachesSimulator)
{
    const ImpairmentScope scope;
    EXPECT_TRUE(Run("net_lag 120"));

    const InstabilitySettings live = InstabilitySimulator::GetInstance().GetSettings();
    EXPECT_TRUE(live.enabled);
    EXPECT_NEAR(live.latencyMs, 120.0f, 1.0e-4f);
    EXPECT_NEAR(EngineSettings::GetInstance().Network().simulatedLatencyMs, 120.0f, 1.0e-4f);

    EXPECT_TRUE(Run("net_jitter 15"));
    EXPECT_NEAR(InstabilitySimulator::GetInstance().GetSettings().jitterMs, 15.0f, 1.0e-4f);
    // Setting jitter re-applies the whole section: latency is still in effect.
    EXPECT_NEAR(InstabilitySimulator::GetInstance().GetSettings().latencyMs, 120.0f, 1.0e-4f);
}

TEST(NetImpairment_NetLossFractionBecomesPercent)
{
    const ImpairmentScope scope;
    EXPECT_TRUE(Run("net_loss 0.05"));

    const InstabilitySettings live = InstabilitySimulator::GetInstance().GetSettings();
    EXPECT_TRUE(live.enabled);
    EXPECT_NEAR(live.packetLossPercent, 5.0f, 1.0e-3f);

    EXPECT_TRUE(Run("net_reorder 30"));
    EXPECT_NEAR(InstabilitySimulator::GetInstance().GetSettings().reorderPercent, 30.0f, 1.0e-3f);
}

TEST(NetImpairment_InvalidArgumentLeavesStateUnchanged)
{
    const ImpairmentScope scope;
    EXPECT_TRUE(Run("net_lag 80"));

    // Non-numeric, trailing garbage, negative, non-finite, out-of-range
    // fraction and a missing argument must all leave the state untouched.
    for (const char* bad : {"net_lag abc", "net_lag 50ms", "net_lag -5", "net_lag nan", "net_lag inf", "net_loss 5",
                            "net_reorder 101", "net_impair_seed -1", "net_lag"})
        Run(bad);

    const InstabilitySettings live = InstabilitySimulator::GetInstance().GetSettings();
    EXPECT_TRUE(live.enabled);
    EXPECT_NEAR(live.latencyMs, 80.0f, 1.0e-4f);
    EXPECT_NEAR(live.packetLossPercent, 0.0f, 1.0e-6f);
    EXPECT_NEAR(live.reorderPercent, 0.0f, 1.0e-6f);
    EXPECT_NEAR(EngineSettings::GetInstance().Network().simulatedLatencyMs, 80.0f, 1.0e-4f);
    EXPECT_EQ(EngineSettings::GetInstance().Network().simulatedImpairmentSeed, 0);
}

TEST(NetImpairment_ImpairOffDisables)
{
    const ImpairmentScope scope;
    EXPECT_TRUE(Run("net_lag 200"));
    EXPECT_TRUE(Run("net_loss 0.2"));
    EXPECT_TRUE(InstabilitySimulator::GetInstance().GetSettings().enabled);

    EXPECT_TRUE(Run("net_impair_off"));
    const InstabilitySettings live = InstabilitySimulator::GetInstance().GetSettings();
    EXPECT_FALSE(live.enabled);
    EXPECT_NEAR(live.latencyMs, 0.0f, 1.0e-6f);
    EXPECT_NEAR(live.packetLossPercent, 0.0f, 1.0e-6f);
    EXPECT_NEAR(EngineSettings::GetInstance().Network().simulatedLatencyMs, 0.0f, 1.0e-6f);
    EXPECT_TRUE(Run("net_impair"));
}

TEST(NetImpairment_SettingsBridgeMapsAllFields)
{
    const ImpairmentScope scope;
    auto& net = EngineSettings::GetInstance().Network();
    net.simulatedLatencyMs = 90.0f;
    net.simulatedJitterMs = 12.0f;
    net.simulatedPacketLoss = 0.25f;
    net.simulatedReorderPercent = 40.0f;
    net.simulatedDuplicatePercent = 10.0f;
    net.simulatedImpairmentSeed = 77;

    InstabilitySettings mapped = Spark::Net::ImpairmentFromEngineSettings(EngineSettings::GetInstance());
    EXPECT_TRUE(mapped.enabled);
    EXPECT_NEAR(mapped.latencyMs, 90.0f, 1.0e-4f);
    EXPECT_NEAR(mapped.jitterMs, 12.0f, 1.0e-4f);
    EXPECT_NEAR(mapped.packetLossPercent, 25.0f, 1.0e-3f);
    EXPECT_NEAR(mapped.reorderPercent, 40.0f, 1.0e-3f);
    EXPECT_NEAR(mapped.duplicatePercent, 10.0f, 1.0e-3f);
    EXPECT_EQ(mapped.seed, uint64_t{77});

    // Clamping: negative / non-finite become zero, fractions above 1 cap at 100%.
    net.simulatedLatencyMs = -10.0f;
    net.simulatedJitterMs = std::numeric_limits<float>::quiet_NaN();
    net.simulatedPacketLoss = 3.0f;
    net.simulatedReorderPercent = 250.0f;
    net.simulatedImpairmentSeed = -5;
    mapped = Spark::Net::ImpairmentFromEngineSettings(EngineSettings::GetInstance());
    EXPECT_EQ(mapped.seed, uint64_t{0});
    EXPECT_NEAR(mapped.latencyMs, 0.0f, 1.0e-6f);
    EXPECT_NEAR(mapped.jitterMs, 0.0f, 1.0e-6f);
    EXPECT_NEAR(mapped.packetLossPercent, 100.0f, 1.0e-3f);
    EXPECT_NEAR(mapped.reorderPercent, 100.0f, 1.0e-3f);

    // All zero means disabled.
    net.simulatedLatencyMs = 0.0f;
    net.simulatedJitterMs = 0.0f;
    net.simulatedPacketLoss = 0.0f;
    net.simulatedReorderPercent = 0.0f;
    net.simulatedDuplicatePercent = 0.0f;
    EXPECT_FALSE(Spark::Net::ImpairmentFromEngineSettings(EngineSettings::GetInstance()).enabled);
}

// A non-zero seed makes an impaired run reproducible: the same seed replays
// the same drop/duplicate/reorder/jitter decisions, and a different seed does not.
TEST(InstabilitySimulator_SeedMakesDecisionsReproducible)
{
    const ImpairmentScope scope;
    auto& simulator = InstabilitySimulator::GetInstance();
    const auto record = [&simulator](uint64_t seed)
    {
        InstabilitySettings settings;
        settings.enabled = true;
        settings.packetLossPercent = 50.0f;
        settings.duplicatePercent = 50.0f;
        settings.reorderPercent = 50.0f;
        settings.latencyMs = 50.0f;
        settings.jitterMs = 25.0f;
        settings.seed = seed;
        simulator.SetSettings(settings);
        std::vector<float> decisions;
        for (int i = 0; i < 64; ++i)
        {
            decisions.push_back(simulator.ShouldDropPacket() ? 1.0f : 0.0f);
            decisions.push_back(simulator.ShouldDuplicate() ? 1.0f : 0.0f);
            decisions.push_back(simulator.ShouldReorder() ? 1.0f : 0.0f);
            decisions.push_back(simulator.GetDelayMs());
        }
        return decisions;
    };

    const std::vector<float> first = record(0xC0FFEEULL);
    const std::vector<float> replay = record(0xC0FFEEULL);
    const std::vector<float> other = record(0xBADF00DULL);

    EXPECT_TRUE(first == replay);
    EXPECT_FALSE(first == other);
}

TEST(NetImpairment_ConsoleSeedAndDuplicateReachSimulator)
{
    const ImpairmentScope scope;
    EXPECT_TRUE(Run("net_dup 25"));
    EXPECT_TRUE(Run("net_impair_seed 4242"));

    const InstabilitySettings live = InstabilitySimulator::GetInstance().GetSettings();
    EXPECT_TRUE(live.enabled);
    EXPECT_NEAR(live.duplicatePercent, 25.0f, 1.0e-3f);
    EXPECT_EQ(live.seed, uint64_t{4242});
}
