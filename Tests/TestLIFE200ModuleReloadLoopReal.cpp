/**
 * @file TestLIFE200ModuleReloadLoopReal.cpp
 * @brief LIFE-200: repeated module load/unload, hot-reload and failed-load loops.
 *
 * Drives the production ModuleManager against the real registry lifecycle
 * fixture image, which registers a host console command and a host
 * InvalidStateDetector rule in OnLoad (and throws after registering when
 * SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD is set). Every loop requires the host
 * registries to return to their exact starting size after each iteration, one
 * registration per live image across hot reloads, no guarded-callback faults,
 * and no thread growth. The surviving command is executed after every reload,
 * so a handler left pointing into an unloaded image faults (or trips ASan)
 * instead of passing. A CycleWatchdog turns a hung FreeLibrary/dlclose or
 * loader lock into a named failure. The Lifecycle_ModuleReload CTest carries
 * the lifecycle label, so the Linux ASan/TSan presets run these loops too.
 */

#include "TestFramework.h"

#include "Core/ModuleManager.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/SparkConsole.h"
#include <Spark/Version.h>

#include "LifecycleLoopGuards.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#ifndef SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH
#error SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH must name the registry lifecycle module fixture
#endif

namespace
{
    constexpr const char* kFixtureModule = "Spark Registry Lifecycle Fixture";
    constexpr const char* kFixtureCommand = "registry_fixture_status";
    constexpr const char* kFixtureRule = "RegistryFixture.Rule";
    constexpr const char* kFixtureRuleCategory = "RegistryFixture";
    constexpr const char* kThrowOnLoadVariable = "SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD";

    constexpr int kLoadUnloadCycles = 50;
    constexpr int kReloadCycles = 20;
    constexpr int kFailedLoadCycles = 20;
    constexpr std::chrono::seconds kCycleDeadline{30};

    class ReloadLoopEngineContext final : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        AudioEngine* GetAudio() override { return nullptr; }
        const AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        Spark::SaveSystem* GetSaveSystem() override { return nullptr; }
        const Spark::SaveSystem* GetSaveSystem() const override { return nullptr; }
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }
    };

    void SetFixtureThrowOnLoad(bool enabled)
    {
#ifdef _WIN32
        _putenv_s(kThrowOnLoadVariable, enabled ? "1" : "");
#else
        if (enabled)
            setenv(kThrowOnLoadVariable, "1", 1);
        else
            unsetenv(kThrowOnLoadVariable);
#endif
    }

    /**
     * Initializes the host console, starts every test from registries without
     * the fixture's entries, and on exit (including an early ASSERT return)
     * clears the throw switch, removes anything a failed iteration left behind
     * and restores the console's prior state.
     */
    struct HostRegistryScope final
    {
        Spark::SimpleConsole& console = Spark::SimpleConsole::GetInstance();
        Spark::InvalidStateDetector& detector = Spark::InvalidStateDetector::GetInstance();
        bool restoreUninitialized = !console.IsInitialized();

        HostRegistryScope()
        {
            if (restoreUninitialized)
                console.Initialize();
            Clear();
        }
        ~HostRegistryScope()
        {
            SetFixtureThrowOnLoad(false);
            Clear();
            if (restoreUninitialized)
                console.Shutdown();
        }

        void Clear()
        {
            console.UnregisterCommand(kFixtureCommand);
            detector.RemoveRulesByCategory(kFixtureRuleCategory);
        }
    };

    /// Shuts down and unloads every module even when an assertion returns early.
    struct ManagerScope final
    {
        ModuleManager& manager;
        ~ManagerScope()
        {
            manager.ShutdownAllAfterPreflight();
            manager.UnloadAll();
        }
    };

    /// Host registry sizes and thread count one iteration must leave unchanged.
    struct HostFootprint
    {
        std::uint32_t consoleCommands = 0;
        std::uint32_t detectorRules = 0;
        std::size_t threads = 0;
    };

    HostFootprint SampleFootprint(const HostRegistryScope& host)
    {
        return {host.console.GetStats().registeredCommands, host.detector.GetRuleCount(),
                LifecycleLoop::CountProcessThreads()};
    }

    /// Registries hold exactly the host entries plus @p fixtureEntries fixture registrations.
    void ExpectFixtureRegistrations(const HostRegistryScope& host, const HostFootprint& empty,
                                    std::uint32_t fixtureEntries)
    {
        EXPECT_EQ(host.console.HasCommand(kFixtureCommand), fixtureEntries != 0);
        EXPECT_EQ(host.detector.HasRule(kFixtureRule), fixtureEntries != 0);
        EXPECT_EQ(host.console.GetStats().registeredCommands, empty.consoleCommands + fixtureEntries);
        EXPECT_EQ(host.detector.GetRuleCount(), empty.detectorRules + fixtureEntries);
    }

    void ExpectNoThreadGrowth(const HostFootprint& baseline)
    {
        EXPECT_LE(LifecycleLoop::CountProcessThreads(), baseline.threads + LifecycleLoop::kOsThreadSlack);
    }
} // namespace

TEST(ModuleReload_RepeatedLoadUnloadRestoresHostRegistries)
{
    HostRegistryScope host;
    ReloadLoopEngineContext context;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);
    const HostFootprint empty = SampleFootprint(host);

    HostFootprint baseline;
    for (int cycle = 0; cycle < kLoadUnloadCycles; ++cycle)
    {
        watchdog.Arm("fixture load/unload cycle " + std::to_string(cycle));
        {
            ModuleManager manager;
            ManagerScope managerScope{manager};
            ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));
            ASSERT_TRUE(manager.InitializeAll(&context));
            ExpectFixtureRegistrations(host, empty, 1);
            EXPECT_TRUE(host.console.ExecuteCommand(kFixtureCommand));

            ASSERT_TRUE(manager.ShutdownAll());
            manager.UnloadAll();
            EXPECT_EQ(manager.GetModuleCount(), std::size_t{0});
            EXPECT_EQ(manager.GetLifecycleEvidence().faults, std::uint64_t{0});
        }
        watchdog.Disarm();

        ExpectFixtureRegistrations(host, empty, 0);
        if (cycle == 0)
        {
            baseline = SampleFootprint(host);
            ASSERT_TRUE(baseline.threads > 0);
            continue;
        }
        ExpectNoThreadGrowth(baseline);
    }
}

TEST(ModuleReload_RepeatedReloadModuleKeepsSingleRegistration)
{
    HostRegistryScope host;
    ReloadLoopEngineContext context;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);
    const HostFootprint empty = SampleFootprint(host);

    ModuleManager manager;
    ManagerScope managerScope{manager};
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));
    ASSERT_TRUE(manager.InitializeAll(&context));
    ExpectFixtureRegistrations(host, empty, 1);

    HostFootprint baseline;
    for (int cycle = 0; cycle < kReloadCycles; ++cycle)
    {
        watchdog.Arm("fixture hot reload " + std::to_string(cycle));
        ASSERT_TRUE(manager.ReloadModule(kFixtureModule, &context));
        watchdog.Disarm();

        // The replacement registered before the outgoing image unloaded: exactly
        // one registration survives, and it dispatches into the live image.
        ASSERT_TRUE(manager.GetModule(kFixtureModule) != nullptr);
        EXPECT_EQ(manager.GetModuleCount(), std::size_t{1});
        ExpectFixtureRegistrations(host, empty, 1);
        EXPECT_TRUE(host.console.ExecuteCommand(kFixtureCommand));
        EXPECT_EQ(manager.GetLifecycleEvidence().faults, std::uint64_t{0});

        if (cycle == 0)
        {
            baseline = SampleFootprint(host);
            ASSERT_TRUE(baseline.threads > 0);
            continue;
        }
        ExpectNoThreadGrowth(baseline);
    }

    ASSERT_TRUE(manager.ShutdownAll());
    ExpectFixtureRegistrations(host, empty, 0);
    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), std::size_t{0});
}

TEST(ModuleReload_ThrowingLoadInLoopLeavesNoResidue)
{
    HostRegistryScope host;
    ReloadLoopEngineContext context;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);
    const HostFootprint empty = SampleFootprint(host);

    HostFootprint baseline;
    for (int cycle = 0; cycle < kFailedLoadCycles; ++cycle)
    {
        // Alternate: an OnLoad that throws after registering, then a clean load
        // of the same image, which must still succeed.
        const bool throwOnLoad = cycle % 2 == 0;
        watchdog.Arm(std::string(throwOnLoad ? "throwing" : "clean") + " fixture load " + std::to_string(cycle));
        {
            ModuleManager manager;
            ManagerScope managerScope{manager};
            ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));

            SetFixtureThrowOnLoad(throwOnLoad);
            bool initialized = throwOnLoad; // the wrong answer, so a skipped call cannot pass
            EXPECT_NO_THROW(initialized = manager.InitializeAll(&context));
            SetFixtureThrowOnLoad(false);

            EXPECT_EQ(initialized, !throwOnLoad);
            EXPECT_EQ(manager.GetModule(kFixtureModule) != nullptr, !throwOnLoad);
            ExpectFixtureRegistrations(host, empty, throwOnLoad ? 0 : 1);
            if (!throwOnLoad)
            {
                EXPECT_TRUE(host.console.ExecuteCommand(kFixtureCommand));
                ASSERT_TRUE(manager.ShutdownAll());
            }
            manager.UnloadAll();
            EXPECT_EQ(manager.GetModuleCount(), std::size_t{0});
        }
        watchdog.Disarm();

        ExpectFixtureRegistrations(host, empty, 0);
        if (cycle == 0)
        {
            baseline = SampleFootprint(host);
            ASSERT_TRUE(baseline.threads > 0);
            continue;
        }
        ExpectNoThreadGrowth(baseline);
    }
}
