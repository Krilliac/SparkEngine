/**
 * @file TestLifecycleCompositionRootFailure.cpp
 * @brief Injected-failure tests for the production LifecycleCompositionRoot (LIFE-200).
 *
 * The composition root is the one live engine init path (InitConsole ->
 * InitDebugSystems -> RunInitialize). These tests drive the shipped class with
 * injected stages and prove it fails closed: a stage that returns false, throws
 * a std::exception, or throws anything else aborts startup, rolls the touched
 * stages back in reverse order (including the partially initialized one), and
 * latches a Failed state that later update/shutdown/initialize calls respect.
 * A throwing Shutdown is contained so the remaining stages still tear down.
 * The last test composes the real production stages with one injected failure
 * and checks the real gameplay services are unpublished by the rollback.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Core/EngineRuntime.h"
#include "Core/Lifecycle/LifecycleCompositionRoot.h"
#include "Core/Lifecycle/LifecycleStages.h"
#include "Engine/ECS/Components.h"
#include "Engine/Events/EventSystem.h"

#include <array>
#include <initializer_list>
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

    enum class InitBehavior
    {
        Succeed,
        ReturnFalse,
        ThrowStdException,
        ThrowUnknown
    };

    enum class ShutdownBehavior
    {
        Succeed,
        ThrowStdException
    };

    struct StageSpec
    {
        std::string name;
        LifecycleOrder order;
        bool initialize = false;
        bool update = false;
        bool shutdown = false;
        InitBehavior initBehavior = InitBehavior::Succeed;
        ShutdownBehavior shutdownBehavior = ShutdownBehavior::Succeed;
    };

    /// Records every call into a shared event log so tests can assert exact order.
    class RecordingStage final : public LifecycleStage
    {
      public:
        RecordingStage(StageSpec spec, std::vector<std::string>& events) : m_spec(std::move(spec)), m_events(events) {}

        std::string_view Name() const override { return m_spec.name; }
        LifecycleOrder Order() const override { return m_spec.order; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsInitialize() const override { return m_spec.initialize; }
        bool SupportsUpdate() const override { return m_spec.update; }
        bool SupportsShutdown() const override { return m_spec.shutdown; }

        bool Initialize() override
        {
            m_events.push_back("init " + m_spec.name);
            switch (m_spec.initBehavior)
            {
            case InitBehavior::Succeed:
                return true;
            case InitBehavior::ReturnFalse:
                return false;
            case InitBehavior::ThrowStdException:
                throw std::runtime_error("injected initialize failure");
            case InitBehavior::ThrowUnknown:
                throw 42;
            }
            return false;
        }

        void Update(float /*dt*/) override { m_events.push_back("update " + m_spec.name); }

        void Shutdown() override
        {
            m_events.push_back("shutdown " + m_spec.name);
            if (m_spec.shutdownBehavior == ShutdownBehavior::ThrowStdException)
                throw std::runtime_error("injected shutdown failure");
        }

      private:
        StageSpec m_spec;
        std::vector<std::string>& m_events;
    };

    /// Four paired init/shutdown stages (A..D), one update-only stage and one
    /// teardown-only stage, mirroring the production shape (InitX + Update + Shutdown).
    /// Specs are listed out of order to prove the root sorts by (Order, Name).
    std::vector<std::unique_ptr<LifecycleStage>> MakeStages(std::vector<std::string>& events,
                                                            InitBehavior stageCInit = InitBehavior::Succeed,
                                                            ShutdownBehavior stageBShutdown = ShutdownBehavior::Succeed)
    {
        std::vector<StageSpec> specs = {
            {"Teardown", LifecycleOrder::Lifecycle, false, false, true},
            {"C", LifecycleOrder::AI, true, false, true, stageCInit},
            {"A", LifecycleOrder::Diagnostics, true, false, true},
            {"Tick", LifecycleOrder::Lifecycle, false, true, false},
            {"D", LifecycleOrder::Audio, true, false, true},
            {"B", LifecycleOrder::Physics, true, false, true, InitBehavior::Succeed, stageBShutdown},
        };

        std::vector<std::unique_ptr<LifecycleStage>> stages;
        for (StageSpec& spec : specs)
            stages.push_back(std::make_unique<RecordingStage>(std::move(spec), events));
        return stages;
    }

    constexpr std::array<RequiredStage, 2> kTestRequiredStages = {
        RequiredStage{"Tick", LifecyclePhase::Update},
        RequiredStage{"Teardown", LifecyclePhase::Shutdown},
    };

    void ExpectEvents(const std::vector<std::string>& actual, std::initializer_list<const char*> expected)
    {
        EXPECT_EQ(actual.size(), expected.size());
        size_t index = 0;
        for (const char* value : expected)
        {
            if (index < actual.size())
                EXPECT_EQ(actual[index], std::string(value));
            ++index;
        }
    }

    /// After a failed startup nothing may run again: no update, no second
    /// teardown, and no re-initialization on top of rolled-back stages.
    void ExpectLatchedFailure(LifecycleCompositionRoot& root, std::vector<std::string>& events)
    {
        EXPECT_TRUE(root.GetState() == LifecycleRootState::Failed);
        const size_t latchedEventCount = events.size();
        root.RunUpdate(1.0f / 60.0f);
        EXPECT_FALSE(root.RunShutdown());
        EXPECT_FALSE(root.RunInitialize());
        EXPECT_EQ(events.size(), latchedEventCount);
        EXPECT_TRUE(root.GetState() == LifecycleRootState::Failed);
    }

    /// Rollback after stage C fails: teardown-only stage first (highest order),
    /// then the failed stage C (it may be partially initialized), then B and A.
    /// D was never attempted, so it is neither initialized nor shut down.
    void ExpectRollbackAfterStageCFailed(const std::vector<std::string>& events)
    {
        ExpectEvents(events,
                     {"init A", "init B", "init C", "shutdown Teardown", "shutdown C", "shutdown B", "shutdown A"});
    }
} // namespace

TEST(PartialInit_CompositionRootFalseReturnRollsBackAndLatches)
{
    std::vector<std::string> events;
    LifecycleCompositionRoot root(MakeStages(events, InitBehavior::ReturnFalse), kTestRequiredStages);
    ASSERT_TRUE(root.IsConfigurationValid());
    EXPECT_TRUE(root.GetState() == LifecycleRootState::Idle);

    EXPECT_FALSE(root.RunInitialize());
    ExpectRollbackAfterStageCFailed(events);
    ExpectLatchedFailure(root, events);
}

TEST(PartialInit_CompositionRootStdExceptionIsContainedAndRolledBack)
{
    std::vector<std::string> events;
    LifecycleCompositionRoot root(MakeStages(events, InitBehavior::ThrowStdException), kTestRequiredStages);
    ASSERT_TRUE(root.IsConfigurationValid());

    // The throw must not escape: startup gets a false return, not an unwind.
    bool escaped = false;
    bool initialized = true;
    try
    {
        initialized = root.RunInitialize();
    }
    catch (...)
    {
        escaped = true;
    }
    EXPECT_FALSE(escaped);
    EXPECT_FALSE(initialized);
    ExpectRollbackAfterStageCFailed(events);
    ExpectLatchedFailure(root, events);
}

TEST(PartialInit_CompositionRootUnknownThrowIsContainedAndRolledBack)
{
    std::vector<std::string> events;
    LifecycleCompositionRoot root(MakeStages(events, InitBehavior::ThrowUnknown), kTestRequiredStages);
    ASSERT_TRUE(root.IsConfigurationValid());

    bool escaped = false;
    bool initialized = true;
    try
    {
        initialized = root.RunInitialize();
    }
    catch (...)
    {
        escaped = true;
    }
    EXPECT_FALSE(escaped);
    EXPECT_FALSE(initialized);
    ExpectRollbackAfterStageCFailed(events);
    ExpectLatchedFailure(root, events);
}

TEST(PartialInit_CompositionRootShutdownThrowIsContainedAndLatched)
{
    std::vector<std::string> events;
    LifecycleCompositionRoot root(MakeStages(events, InitBehavior::Succeed, ShutdownBehavior::ThrowStdException),
                                  kTestRequiredStages);
    ASSERT_TRUE(root.IsConfigurationValid());

    ASSERT_TRUE(root.RunInitialize());
    EXPECT_TRUE(root.GetState() == LifecycleRootState::Initialized);
    root.RunUpdate(1.0f / 60.0f);

    bool escaped = false;
    bool clean = true;
    try
    {
        clean = root.RunShutdown();
    }
    catch (...)
    {
        escaped = true;
    }
    EXPECT_FALSE(escaped);
    EXPECT_FALSE(clean);

    // B's throw does not strand A: every stage still tears down, in reverse order.
    ExpectEvents(events, {"init A", "init B", "init C", "init D", "update Tick", "shutdown Teardown", "shutdown D",
                          "shutdown C", "shutdown B", "shutdown A"});
    ExpectLatchedFailure(root, events);
}

TEST(PartialInit_CompositionRootRollbackThrowStillUnwindsEarlierStages)
{
    std::vector<std::string> events;
    LifecycleCompositionRoot root(MakeStages(events, InitBehavior::ReturnFalse, ShutdownBehavior::ThrowStdException),
                                  kTestRequiredStages);
    ASSERT_TRUE(root.IsConfigurationValid());

    EXPECT_FALSE(root.RunInitialize());
    ExpectRollbackAfterStageCFailed(events);
    ExpectLatchedFailure(root, events);
}

TEST(PartialInit_CompositionRootInvalidCompositionFailsClosed)
{
    // A required stage that is missing must stop startup before any stage runs,
    // instead of silently disabling the lifecycle and reporting success.
    std::vector<std::string> events;
    constexpr std::array<RequiredStage, 1> kMissingStage = {RequiredStage{"InitMissing", LifecyclePhase::Initialize}};
    LifecycleCompositionRoot missing(MakeStages(events), kMissingStage);
    EXPECT_FALSE(missing.IsConfigurationValid());
    EXPECT_FALSE(missing.RunInitialize());
    ExpectLatchedFailure(missing, events);
    EXPECT_TRUE(events.empty());

    // A required stage that exists but is not wired for the phase is invalid too.
    constexpr std::array<RequiredStage, 1> kUnwiredPhase = {RequiredStage{"Tick", LifecyclePhase::Shutdown}};
    LifecycleCompositionRoot unwired(MakeStages(events), kUnwiredPhase);
    EXPECT_FALSE(unwired.IsConfigurationValid());
    EXPECT_FALSE(unwired.RunInitialize());
    EXPECT_TRUE(events.empty());

    std::vector<std::unique_ptr<LifecycleStage>> withNull = MakeStages(events);
    withNull.push_back(nullptr);
    LifecycleCompositionRoot nullStage(std::move(withNull), kTestRequiredStages);
    EXPECT_FALSE(nullStage.IsConfigurationValid());
    EXPECT_FALSE(nullStage.RunInitialize());
    ExpectLatchedFailure(nullStage, events);
    EXPECT_TRUE(events.empty());
}

TEST(PartialInit_CompositionRootCleanCycleShutsDownOnceAndReinitializes)
{
    std::vector<std::string> events;
    LifecycleCompositionRoot root(MakeStages(events), kTestRequiredStages);
    ASSERT_TRUE(root.IsConfigurationValid());

    ASSERT_TRUE(root.RunInitialize());
    EXPECT_TRUE(root.RunInitialize()); // idempotent while initialized
    root.RunUpdate(1.0f / 60.0f);
    EXPECT_TRUE(root.RunShutdown());
    EXPECT_TRUE(root.GetState() == LifecycleRootState::ShutDown);
    ExpectEvents(events, {"init A", "init B", "init C", "init D", "update Tick", "shutdown Teardown", "shutdown D",
                          "shutdown C", "shutdown B", "shutdown A"});

    // Shut down: no update, and a second shutdown is a clean no-op.
    const size_t shutDownEventCount = events.size();
    root.RunUpdate(1.0f / 60.0f);
    EXPECT_TRUE(root.RunShutdown());
    EXPECT_EQ(events.size(), shutDownEventCount);

    // A clean shutdown allows a fresh initialize (editor/test restart).
    events.clear();
    ASSERT_TRUE(root.RunInitialize());
    EXPECT_TRUE(root.RunShutdown());
    ExpectEvents(events, {"init A", "init B", "init C", "init D", "shutdown Teardown", "shutdown D", "shutdown C",
                          "shutdown B", "shutdown A"});
}

TEST(PartialInit_CompositionRootProductionStagesRollBackOnInjectedFailure)
{
    // Platform startup provides a context with a World and an EventBus before
    // the lifecycle runs; statics keep the pointers valid for later tests.
    if (!EngineContext::Get())
        EngineContext::SetOwned(std::make_unique<EngineContext>());
    static World s_world;
    static Spark::EventBus s_bus;
    EngineContext* ctx = EngineContext::Get();
    ctx->SetWorld(&s_world);
    ctx->SetEventBus(&s_bus);

    // The production composition itself must validate, or every boot fails.
    EXPECT_TRUE(LifecycleCompositionRoot::Get().IsConfigurationValid());

    // Real production stages plus one injected stage that fails after gameplay
    // init has published its services (Render orders after every init stage).
    std::vector<std::string> events;
    std::vector<std::unique_ptr<LifecycleStage>> stages;
    stages.push_back(Spark::Core::Lifecycle::CreateInitDebugStage());
    stages.push_back(Spark::Core::Lifecycle::CreateInitNetworkingStage());
    stages.push_back(Spark::Core::Lifecycle::CreateInitGameplayStage());
    stages.push_back(Spark::Core::Lifecycle::CreateUpdateStage());
    stages.push_back(Spark::Core::Lifecycle::CreateShutdownStage());
    stages.push_back(std::make_unique<RecordingStage>(
        StageSpec{"InjectedFailure", LifecycleOrder::Render, true, false, false, InitBehavior::ThrowStdException},
        events));

    constexpr std::array<RequiredStage, 5> kProductionShape = {
        RequiredStage{"InitNetworking", LifecyclePhase::Initialize},
        RequiredStage{"InitGameplay", LifecyclePhase::Initialize},
        RequiredStage{"InitDebug", LifecyclePhase::Initialize},
        RequiredStage{"Update", LifecyclePhase::Update},
        RequiredStage{"Shutdown", LifecyclePhase::Shutdown},
    };
    LifecycleCompositionRoot root(std::move(stages), kProductionShape);
    ASSERT_TRUE(root.IsConfigurationValid());

    EXPECT_FALSE(root.RunInitialize());
    ExpectEvents(events, {"init InjectedFailure"});
    EXPECT_TRUE(root.GetState() == LifecycleRootState::Failed);

    // The real Shutdown stage ran as part of the rollback: gameplay and
    // diagnostic services published during init are no longer reachable.
    EXPECT_TRUE(ctx->GetConditions() == nullptr);
    EXPECT_TRUE(ctx->GetAbilities() == nullptr);
    EXPECT_TRUE(ctx->GetAI() == nullptr);
    EXPECT_TRUE(ctx->GetWeapons() == nullptr);
    EXPECT_TRUE(GetEngineRuntime().weaponSystem == nullptr);
    EXPECT_TRUE(ctx->GetComponentSerializers() == nullptr);
    EXPECT_TRUE(ctx->GetInvalidStateDetector() == nullptr);

    // Latched: the production update/shutdown stages never run on the rolled-back state.
    EXPECT_FALSE(root.RunShutdown());
    EXPECT_FALSE(root.RunInitialize());
    EXPECT_TRUE(ctx->GetConditions() == nullptr);
}
