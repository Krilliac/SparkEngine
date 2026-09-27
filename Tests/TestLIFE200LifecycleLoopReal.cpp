/**
 * @file TestLIFE200LifecycleLoopReal.cpp
 * @brief LIFE-200: repeated boot/shutdown loops of the production lifecycle stages.
 *
 * Every cycle builds a fresh LifecycleCompositionRoot from the real stage
 * factories (the same five-stage shape the engine boots with) and drives
 * initialize -> update -> shutdown against a live EngineContext. A cycle must
 * publish the engine-lifetime services, withdraw every one of them at
 * shutdown, and leave the host console registry, the ECS phase pipeline and
 * the process thread count exactly where a fresh warm-up boot left them. The
 * same loop runs again after a lifecycle someone booted and never shut down,
 * whose running debug systems must not skew that baseline. Another loop
 * alternates a rolled-back startup with a clean one, so a failed boot can
 * neither latch the services dead nor leak what it had already started.
 *
 * A fault test injects a failing stage at every boundary between the
 * production init stages (before InitDebug, after InitDebug, after
 * InitNetworking, after InitGameplay), failing by returning false, throwing a
 * std::exception and throwing a non-std value; each rolled-back boot must
 * leave no published service, ECS phase system, console command or thread
 * behind, keep its root latched, and let the next boot start cleanly. Another
 * proves an Update stage that throws escapes RunUpdate without poisoning the
 * root: every such cycle still shuts down to the same footprint. The last boots
 * beside a script engine another owner started: each boot must publish and
 * withdraw its own engine, never adopt or shut down the foreign one, so the
 * script console commands a boot registers do not depend on test order.
 *
 * A CycleWatchdog bounds every cycle: a deadlock aborts the run with the cycle
 * named instead of hanging until the CTest timeout. The Lifecycle_LifecycleLoop
 * CTest carries the lifecycle label, so the Linux ASan/TSan presets run these
 * loops under the sanitizers.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Core/EngineRuntime.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Core/Lifecycle/LifecycleCompositionRoot.h"
#include "Core/Lifecycle/LifecycleStages.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/PhaseSystemManager.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Utils/DebugOverlay.h"
#include "Utils/SparkConsole.h"

#include "LifecycleLoopGuards.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Spark::Core::Lifecycle::LifecycleCompositionRoot;
    using Spark::Core::Lifecycle::LifecycleOrder;
    using Spark::Core::Lifecycle::LifecyclePhase;
    using Spark::Core::Lifecycle::LifecycleRootState;
    using Spark::Core::Lifecycle::LifecycleStage;
    using Spark::Core::Lifecycle::LifecycleThreadAffinity;
    using Spark::Core::Lifecycle::RequiredStage;

    constexpr int kBootCycles = 25;
    constexpr int kAlternatingRounds = 10;
    constexpr int kUpdatesPerCycle = 3;
    // Generous for TSan/ASan Debug builds; a real deadlock never finishes at all.
    constexpr std::chrono::seconds kCycleDeadline{30};

    constexpr std::array<RequiredStage, 5> kProductionShape = {
        RequiredStage{"InitNetworking", LifecyclePhase::Initialize},
        RequiredStage{"InitGameplay", LifecyclePhase::Initialize},
        RequiredStage{"InitDebug", LifecyclePhase::Initialize},
        RequiredStage{"Update", LifecyclePhase::Update},
        RequiredStage{"Shutdown", LifecyclePhase::Shutdown},
    };

    /// How an injected stage fails the phase it takes part in.
    enum class InjectedFault : std::uint8_t
    {
        ReturnFalse,
        ThrowStdException,
        ThrowUnknown
    };

    /// A stage that fails its Initialize (or throws from its Update) at a chosen
    /// (Order, Name) position between the production stages.
    class FaultStage final : public LifecycleStage
    {
      public:
        FaultStage(LifecycleOrder order, std::string_view name, LifecyclePhase phase, InjectedFault fault,
                   int* invocations)
            : m_order(order), m_name(name), m_phase(phase), m_fault(fault), m_invocations(invocations)
        {
        }

        std::string_view Name() const override { return m_name; }
        LifecycleOrder Order() const override { return m_order; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsInitialize() const override { return m_phase == LifecyclePhase::Initialize; }
        bool SupportsUpdate() const override { return m_phase == LifecyclePhase::Update; }

        bool Initialize() override
        {
            Fire();
            return false;
        }

        void Update(float /*dt*/) override { Fire(); }

      private:
        void Fire()
        {
            if (m_invocations != nullptr)
                ++*m_invocations;
            switch (m_fault)
            {
            case InjectedFault::ReturnFalse:
                return;
            case InjectedFault::ThrowStdException:
                throw std::runtime_error("injected lifecycle stage failure");
            case InjectedFault::ThrowUnknown:
                throw 42;
            }
        }

        LifecycleOrder m_order;
        std::string_view m_name;
        LifecyclePhase m_phase;
        InjectedFault m_fault;
        int* m_invocations;
    };

    /// A fresh root over the real production stages, plus an optional injected stage.
    std::unique_ptr<LifecycleCompositionRoot> MakeProductionRoot(std::unique_ptr<LifecycleStage> injected = nullptr)
    {
        std::vector<std::unique_ptr<LifecycleStage>> stages;
        stages.push_back(Spark::Core::Lifecycle::CreateInitDebugStage());
        stages.push_back(Spark::Core::Lifecycle::CreateInitNetworkingStage());
        stages.push_back(Spark::Core::Lifecycle::CreateInitGameplayStage());
        stages.push_back(Spark::Core::Lifecycle::CreateUpdateStage());
        stages.push_back(Spark::Core::Lifecycle::CreateShutdownStage());
        if (injected)
            stages.push_back(std::move(injected));
        return std::make_unique<LifecycleCompositionRoot>(std::move(stages), kProductionShape);
    }

    /// Fails startup after every production init stage has published its services.
    std::unique_ptr<LifecycleCompositionRoot> MakeFailingTailRoot()
    {
        return MakeProductionRoot(std::make_unique<FaultStage>(LifecycleOrder::Render, "InjectedFailure",
                                                               LifecyclePhase::Initialize, InjectedFault::ReturnFalse,
                                                               nullptr));
    }

    /// Platform startup provides a World and an EventBus before the lifecycle runs.
    EngineContext* PrepareContext()
    {
        if (!EngineContext::Get())
            EngineContext::SetOwned(std::make_unique<EngineContext>());
        static World s_world;
        static Spark::EventBus s_bus;
        EngineContext* ctx = EngineContext::Get();
        ctx->SetWorld(&s_world);
        ctx->SetEventBus(&s_bus);
        return ctx;
    }

    /// Initializes the host console so lifecycle command registrations are counted.
    struct ConsoleScope final
    {
        Spark::SimpleConsole& console = Spark::SimpleConsole::GetInstance();
        bool restoreUninitialized = !console.IsInitialized();
        ConsoleScope()
        {
            if (restoreUninitialized)
                console.Initialize();
        }
        ~ConsoleScope()
        {
            if (restoreUninitialized)
                console.Shutdown();
        }
    };

    /// Everything a cycle may not accumulate, sampled after its shutdown.
    struct CycleFootprint
    {
        std::uint32_t consoleCommands = 0;
        std::size_t threads = 0;
    };

    CycleFootprint SampleFootprint(const Spark::SimpleConsole& console)
    {
        return {console.GetStats().registeredCommands, LifecycleLoop::CountProcessThreads()};
    }

    bool ServicesPublished(EngineContext& ctx)
    {
        // The published script engine is the running singleton, never a stale
        // pointer left by an earlier boot beside some other owner's engine.
        return ctx.GetConditions() != nullptr && ctx.GetAbilities() != nullptr && ctx.GetAI() != nullptr &&
               ctx.GetWeapons() != nullptr && ctx.GetWeapons() == GetEngineRuntime().weaponSystem.get() &&
               ctx.GetComponentSerializers() != nullptr && ctx.GetInvalidStateDetector() != nullptr &&
               ctx.GetScriptEngine() != nullptr && ctx.GetScriptEngine() == AngelScriptEngine::GetInstance();
    }

    bool ServicesWithdrawn(EngineContext& ctx)
    {
        return ctx.GetConditions() == nullptr && ctx.GetAbilities() == nullptr && ctx.GetAI() == nullptr &&
               ctx.GetWeapons() == nullptr && GetEngineRuntime().weaponSystem == nullptr &&
               ctx.GetComponentSerializers() == nullptr && ctx.GetInvalidStateDetector() == nullptr &&
               ctx.GetScriptEngine() == nullptr;
    }

    std::size_t PhaseSystemCount()
    {
        return Spark::Core::Lifecycle::GetPhaseSystemManagerImpl().GetSystemCount();
    }

    /// Boots and shuts down a clean production root; returns the footprint it leaves.
    CycleFootprint CleanCycle(EngineContext& ctx, const Spark::SimpleConsole& console)
    {
        auto root = MakeProductionRoot();
        EXPECT_TRUE(root->RunInitialize());
        EXPECT_TRUE(ServicesPublished(ctx));
        root->RunUpdate(1.0f / 60.0f);
        EXPECT_TRUE(root->RunShutdown());
        EXPECT_TRUE(ServicesWithdrawn(ctx));
        EXPECT_EQ(PhaseSystemCount(), std::size_t{0});
        root.reset();
        return SampleFootprint(console);
    }

    /// Two warm-up boots; returns the footprint of the second, the first fresh one.
    ///
    /// The first boot starts process-lifetime state (lazy singletons, pools) and
    /// retires a lifecycle an earlier caller may have left running. Such a boot
    /// finds that caller's debug systems and published script engine already up,
    /// so it registers none of their console commands (memory.integrity.*,
    /// sandbox.*), yet its shutdown really stops them. Only the second boot
    /// initializes everything afresh, as every boot after it will.
    CycleFootprint FreshBootBaseline(EngineContext& ctx, const Spark::SimpleConsole& console,
                                     LifecycleLoop::CycleWatchdog& watchdog, std::string_view before)
    {
        watchdog.Arm("warm-up boot retiring any inherited lifecycle before " + std::string(before));
        CleanCycle(ctx, console);
        watchdog.Arm("fresh warm-up boot before " + std::string(before));
        const CycleFootprint baseline = CleanCycle(ctx, console);
        watchdog.Disarm();
        return baseline;
    }

    /// The body of the repeated boot/shutdown loop: kBootCycles production boots,
    /// each of which must publish, withdraw and leave the fresh warm-up's footprint.
    void RunRepeatedBootShutdownLoop(EngineContext& ctx, const Spark::SimpleConsole& console)
    {
        LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);

        const CycleFootprint baseline = FreshBootBaseline(ctx, console, watchdog, "repeated boot/shutdown cycles");
        ASSERT_TRUE(baseline.threads > 0);

        std::size_t baselinePhaseSystems = 0;
        for (int cycle = 0; cycle < kBootCycles; ++cycle)
        {
            watchdog.Arm("production boot/shutdown cycle " + std::to_string(cycle));
            auto root = MakeProductionRoot();
            ASSERT_TRUE(root->IsConfigurationValid());
            ASSERT_TRUE(root->RunInitialize());
            EXPECT_TRUE(ServicesPublished(ctx));

            // The phase pipeline is rebuilt per boot, never accumulated across boots.
            const std::size_t phaseSystems = PhaseSystemCount();
            if (cycle == 0)
                baselinePhaseSystems = phaseSystems;
            EXPECT_TRUE(phaseSystems > 0);
            EXPECT_EQ(phaseSystems, baselinePhaseSystems);

            for (int frame = 0; frame < kUpdatesPerCycle; ++frame)
                root->RunUpdate(1.0f / 60.0f);

            ASSERT_TRUE(root->RunShutdown());
            EXPECT_TRUE(root->GetState() == LifecycleRootState::ShutDown);
            EXPECT_TRUE(ServicesWithdrawn(ctx));
            EXPECT_EQ(PhaseSystemCount(), std::size_t{0});
            // The debug stage enabled and ticked the overlay singleton; shutdown undoes both.
            EXPECT_FALSE(Spark::DebugOverlay::GetInstance().IsEnabled());
            EXPECT_EQ(Spark::DebugOverlay::GetInstance().GetFrameCount(), std::uint64_t{0});
            root.reset();
            watchdog.Disarm();

            // Every cycle leaves exactly the fresh warm-up's footprint behind.
            const CycleFootprint footprint = SampleFootprint(console);
            EXPECT_EQ(footprint.consoleCommands, baseline.consoleCommands);
            EXPECT_LE(footprint.threads, baseline.threads + LifecycleLoop::kOsThreadSlack);
        }
    }
} // namespace

TEST(LifecycleLoop_ProductionStagesRepeatedBootShutdownLeavesNoStaleServices)
{
    EngineContext* ctx = PrepareContext();
    ConsoleScope consoleScope;
    RunRepeatedBootShutdownLoop(*ctx, consoleScope.console);
}

TEST(LifecycleLoop_RepeatedBootsAfterAnUnfinishedLifecycleKeepOneFootprint)
{
    // An earlier caller booted the debug and gameplay systems and never shut
    // them down, before the host console was up (a test fixture that only
    // tears down gameplay does this). Its debug systems and published script
    // engine stay up, so the next boot registers none of their console commands
    // (memory.integrity.*, sandbox.*) but its shutdown stops them; a baseline
    // sampled after that boot sees them appear on every boot after it.
    EngineContext* ctx = PrepareContext();
    ASSERT_TRUE(Spark::Core::Lifecycle::InitializeDebugSystemsImpl());
    ASSERT_TRUE(Spark::Core::Lifecycle::InitializeGameplaySystemsImpl());

    ConsoleScope consoleScope;
    RunRepeatedBootShutdownLoop(*ctx, consoleScope.console);
}

TEST(LifecycleLoop_FailedBootThenCleanBootRepublishesServices)
{
    EngineContext* ctx = PrepareContext();
    ConsoleScope consoleScope;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);

    CycleFootprint baseline;
    for (int round = 0; round < kAlternatingRounds; ++round)
    {
        // A startup that fails after every production stage initialized rolls
        // back through the real Shutdown stage and latches only its own root.
        watchdog.Arm("failed boot in round " + std::to_string(round));
        auto failed = MakeFailingTailRoot();
        ASSERT_TRUE(failed->IsConfigurationValid());
        EXPECT_FALSE(failed->RunInitialize());
        EXPECT_TRUE(failed->GetState() == LifecycleRootState::Failed);
        EXPECT_TRUE(ServicesWithdrawn(*ctx));
        failed.reset();

        // The next boot on the same context starts from scratch and republishes.
        watchdog.Arm("clean boot after failure in round " + std::to_string(round));
        auto clean = MakeProductionRoot();
        ASSERT_TRUE(clean->RunInitialize());
        EXPECT_TRUE(ServicesPublished(*ctx));
        clean->RunUpdate(1.0f / 60.0f);
        ASSERT_TRUE(clean->RunShutdown());
        EXPECT_TRUE(ServicesWithdrawn(*ctx));
        clean.reset();
        watchdog.Disarm();

        const CycleFootprint footprint = SampleFootprint(consoleScope.console);
        if (round == 0)
        {
            baseline = footprint;
            ASSERT_TRUE(baseline.threads > 0);
            continue;
        }
        EXPECT_EQ(footprint.consoleCommands, baseline.consoleCommands);
        EXPECT_LE(footprint.threads, baseline.threads + LifecycleLoop::kOsThreadSlack);
    }
}

namespace
{
    /// A slot between production init stages, pinned by the root's (Order, Name) sort.
    struct InjectionPoint
    {
        LifecycleOrder order;
        std::string_view name;
        std::string_view where;
    };

    constexpr std::array<InjectionPoint, 4> kInitBoundaries = {
        InjectionPoint{LifecycleOrder::Diagnostics, "0Inject", "before InitDebug"},
        InjectionPoint{LifecycleOrder::Physics, "Inject", "after InitDebug"},
        InjectionPoint{LifecycleOrder::AI, "ZInject", "after InitNetworking"},
        InjectionPoint{LifecycleOrder::Render, "Inject", "after InitGameplay"},
    };

    struct FaultCase
    {
        InjectedFault fault;
        std::string_view name;
    };

    constexpr std::array<FaultCase, 3> kFaults = {
        FaultCase{InjectedFault::ReturnFalse, "return false"},
        FaultCase{InjectedFault::ThrowStdException, "throw std::exception"},
        FaultCase{InjectedFault::ThrowUnknown, "throw non-std value"},
    };

    /// True when RunUpdate let a stage exception reach the caller.
    bool UpdateEscaped(LifecycleCompositionRoot& root)
    {
        try
        {
            root.RunUpdate(1.0f / 60.0f);
        }
        catch (...)
        {
            return true;
        }
        return false;
    }
} // namespace

TEST(LifecycleLoop_InjectedFailureAtEveryInitBoundaryUnwinds)
{
    EngineContext* ctx = PrepareContext();
    ConsoleScope consoleScope;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);

    const CycleFootprint baseline = FreshBootBaseline(*ctx, consoleScope.console, watchdog, "fault injection");
    ASSERT_TRUE(baseline.threads > 0);

    for (const InjectionPoint& point : kInitBoundaries)
    {
        for (const FaultCase& faultCase : kFaults)
        {
            const std::string label = std::string(faultCase.name) + " " + std::string(point.where);
            watchdog.Arm("injected failure: " + label);

            int invocations = 0;
            auto failed = MakeProductionRoot(std::make_unique<FaultStage>(
                point.order, point.name, LifecyclePhase::Initialize, faultCase.fault, &invocations));
            ASSERT_TRUE(failed->IsConfigurationValid());
            EXPECT_FALSE(failed->RunInitialize());
            EXPECT_EQ(invocations, 1);
            EXPECT_TRUE(failed->GetState() == LifecycleRootState::Failed);

            // Rollback withdrew whatever the stages before the fault had published.
            EXPECT_TRUE(ServicesWithdrawn(*ctx));
            EXPECT_EQ(PhaseSystemCount(), std::size_t{0});
            const CycleFootprint afterRollback = SampleFootprint(consoleScope.console);
            EXPECT_EQ(afterRollback.consoleCommands, baseline.consoleCommands);
            EXPECT_LE(afterRollback.threads, baseline.threads + LifecycleLoop::kOsThreadSlack);

            // The latched root refuses both transitions and runs no stage again.
            EXPECT_FALSE(failed->RunShutdown());
            EXPECT_FALSE(failed->RunInitialize());
            EXPECT_EQ(invocations, 1);
            EXPECT_TRUE(failed->GetState() == LifecycleRootState::Failed);
            EXPECT_TRUE(ServicesWithdrawn(*ctx));
            EXPECT_EQ(SampleFootprint(consoleScope.console).consoleCommands, baseline.consoleCommands);
            failed.reset();

            // No stale singleton, thread or registration blocks the next boot.
            watchdog.Arm("clean boot after injected failure: " + label);
            const CycleFootprint afterClean = CleanCycle(*ctx, consoleScope.console);
            watchdog.Disarm();
            EXPECT_EQ(afterClean.consoleCommands, baseline.consoleCommands);
            EXPECT_LE(afterClean.threads, baseline.threads + LifecycleLoop::kOsThreadSlack);
        }
    }
}

TEST(LifecycleLoop_ThrowingUpdateStageDoesNotLeakAcrossCycles)
{
    EngineContext* ctx = PrepareContext();
    ConsoleScope consoleScope;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);

    const CycleFootprint baseline = FreshBootBaseline(*ctx, consoleScope.console, watchdog, "throwing updates");
    ASSERT_TRUE(baseline.threads > 0);

    for (int cycle = 0; cycle < kAlternatingRounds; ++cycle)
    {
        watchdog.Arm("throwing update cycle " + std::to_string(cycle));
        const InjectedFault fault = (cycle % 2 == 0) ? InjectedFault::ThrowStdException : InjectedFault::ThrowUnknown;
        int invocations = 0;
        auto root = MakeProductionRoot(std::make_unique<FaultStage>(LifecycleOrder::Render, "ThrowingUpdate",
                                                                    LifecyclePhase::Update, fault, &invocations));
        ASSERT_TRUE(root->RunInitialize());
        EXPECT_TRUE(ServicesPublished(*ctx));

        // RunUpdate is not a containment boundary: the host's frame loop sees the
        // exception. The root itself stays Initialized and keeps its services.
        for (int frame = 0; frame < kUpdatesPerCycle; ++frame)
            EXPECT_TRUE(UpdateEscaped(*root));
        EXPECT_EQ(invocations, kUpdatesPerCycle);
        EXPECT_TRUE(root->GetState() == LifecycleRootState::Initialized);
        EXPECT_TRUE(ServicesPublished(*ctx));

        // Shutdown after the throwing frames is complete and clean.
        ASSERT_TRUE(root->RunShutdown());
        EXPECT_TRUE(root->GetState() == LifecycleRootState::ShutDown);
        EXPECT_TRUE(ServicesWithdrawn(*ctx));
        EXPECT_EQ(PhaseSystemCount(), std::size_t{0});
        root.reset();
        watchdog.Disarm();

        const CycleFootprint footprint = SampleFootprint(consoleScope.console);
        EXPECT_EQ(footprint.consoleCommands, baseline.consoleCommands);
        EXPECT_LE(footprint.threads, baseline.threads + LifecycleLoop::kOsThreadSlack);
    }
}

namespace
{
    /// A script engine some other owner started. Shut down on scope exit, so a
    /// failed assertion never leaves GetInstance() naming a destroyed engine.
    struct ForeignScriptEngine final
    {
        AngelScriptEngine engine;
        ~ForeignScriptEngine() { engine.Shutdown(); }
    };
} // namespace

TEST(LifecycleLoop_ForeignScriptEngineIsNeitherAdoptedNorShutDown)
{
    EngineContext* ctx = PrepareContext();
    ConsoleScope consoleScope;
    LifecycleLoop::CycleWatchdog watchdog(kCycleDeadline);

    // The warm-up shutdowns must withdraw the script service, or the next boot
    // takes the stale pointer for a running engine of its own.
    const CycleFootprint baseline =
        FreshBootBaseline(*ctx, consoleScope.console, watchdog, "a foreign script engine starts");

    ForeignScriptEngine foreign;
    ASSERT_TRUE(foreign.engine.Initialize());
    ASSERT_TRUE(AngelScriptEngine::GetInstance() == &foreign.engine);

    for (int cycle = 0; cycle < kAlternatingRounds; ++cycle)
    {
        watchdog.Arm("boot beside a foreign script engine, cycle " + std::to_string(cycle));
        auto root = MakeProductionRoot();
        ASSERT_TRUE(root->RunInitialize());
        // The boot starts and publishes its own engine rather than adopting the
        // foreign one, so every boot registers the same script console commands.
        EXPECT_TRUE(ServicesPublished(*ctx));
        EXPECT_TRUE(ctx->GetScriptEngine() != &foreign.engine);
        ASSERT_TRUE(root->RunShutdown());
        EXPECT_TRUE(ServicesWithdrawn(*ctx));
        root.reset();
        watchdog.Disarm();

        EXPECT_EQ(SampleFootprint(consoleScope.console).consoleCommands, baseline.consoleCommands);
#ifdef SPARK_ANGELSCRIPT_SUPPORT
        // The lifecycle's shutdown released only its own engine: the foreign one still compiles.
        EXPECT_TRUE(foreign.engine.CompileScriptFromString("void ForeignProbe() {}\n",
                                                           "LifecycleForeignProbe" + std::to_string(cycle)));
#endif
    }
}
