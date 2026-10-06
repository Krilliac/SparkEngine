/**
 * @file TestEngineWiringReal.cpp
 * @brief Regression tests for the reserved-file engine wiring landed in the
 *        release-readiness sweep: the production lifecycle must register its
 *        engine-lifetime services on EngineContext, tick the systems it
 *        initializes, and order the diagnostics stage first.
 *
 * Every test drives the exact production entry points
 * (Core/Lifecycle/GameplayLifecycleShared.h) against the process-wide
 * EngineContext, headless (no graphics, no audio device, no window), so a
 * regression that un-wires a service or a tick fails here without a GPU.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Core/EngineRuntime.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Core/Lifecycle/LifecycleStages.h"
#include "Engine/AI/AISystem.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/Localization/LocalizationSystem.h"
#include "Engine/Rendering/MovieRenderPipeline.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Engine/World/ProximityTriggerSystem.h"
#include "Input/InputActionSystem.h"
#include "Input/InputManager.h"
#include "Utils/LogMacros.h"
#include "Utils/Logger.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    // The process-wide EngineContext with a World and an EventBus, exactly as
    // platform startup provides them before the lifecycle stages run. Statics so
    // the context never points at a dead frame object across tests.
    World& SetupContextWithWorld()
    {
        if (!EngineContext::Get())
        {
            EngineContext::SetOwned(std::make_unique<EngineContext>());
        }
        static World s_world;
        static Spark::EventBus s_bus;
        EngineContext::Get()->SetWorld(&s_world);
        EngineContext::Get()->SetEventBus(&s_bus);
        return s_world;
    }

    Spark::EventBus& ContextEventBus()
    {
        return *EngineContext::Get()->GetEventBus();
    }

    /// Whole-file read used to prove the log file actually received a line the
    /// production macro wrote. FileSink auto-flushes at Error level and opens the
    /// file with shared reads, so the marker is on disk by the time this runs.
    std::string ReadWholeFile(const std::string& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {};
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    void InitializeProductionLifecycle()
    {
        // Same order the composition root uses: diagnostics first, then gameplay.
        // Both entry points are re-runnable (the phase manager is rebuilt).
        Spark::Core::Lifecycle::InitializeDebugSystemsImpl();
        Spark::Core::Lifecycle::InitializeGameplaySystemsImpl();
    }
} // namespace

// ============================================================================
// core-05: InitDebug must run before networking/gameplay init
// ============================================================================

TEST(EngineWiring_InitDebugStageRunsBeforeEveryOtherStage)
{
    using Spark::Core::Lifecycle::LifecycleOrder;
    const auto debugStage = Spark::Core::Lifecycle::CreateInitDebugStage();
    const auto networkingStage = Spark::Core::Lifecycle::CreateInitNetworkingStage();
    const auto gameplayStage = Spark::Core::Lifecycle::CreateInitGameplayStage();

    ASSERT_TRUE(debugStage != nullptr);
    EXPECT_TRUE(debugStage->Order() == LifecycleOrder::Diagnostics);
    EXPECT_TRUE(static_cast<uint16_t>(debugStage->Order()) < static_cast<uint16_t>(networkingStage->Order()));
    EXPECT_TRUE(static_cast<uint16_t>(debugStage->Order()) < static_cast<uint16_t>(gameplayStage->Order()));
    EXPECT_TRUE(static_cast<uint16_t>(LifecycleOrder::Diagnostics) < static_cast<uint16_t>(LifecycleOrder::Physics));
}

// ============================================================================
// utils-01/utils-03: standard Logger sinks, installed once per process
// ============================================================================

TEST(EngineWiring_LogSinksInstallExactlyOncePerProcess)
{
    const std::string first = Spark::Core::Lifecycle::InstallEngineLogSinksImpl();

    // The returned path is the artifact the release gates read. Comparing the two
    // calls with each other (or with runtime.engineLogPath, which is where the
    // return value comes from) holds even when the file sink never opened and all
    // three are empty, so the file itself is the assertion.
    ASSERT_FALSE(first.empty());
    EXPECT_TRUE(GetEngineRuntime().logSinksInstalled);
    std::error_code pathError;
    ASSERT_TRUE(std::filesystem::exists(first, pathError));

    // A file sink is actually installed and receiving: write a marker through the
    // production macro and read it back. Without this the test cannot tell
    // "installed" from "the Logger has a stderr sink and nothing else".
    // The current file size is a token that cannot already appear in the file:
    // the sink only ever appends, so nothing was written at this offset yet.
    const auto sizeBeforeMarker = std::filesystem::file_size(first, pathError);
    ASSERT_TRUE(!pathError);
    const std::string marker = "EngineWiring_LogSinkProbe_" + std::to_string(sizeBeforeMarker);
    // Info, not Error: TestMain deliberately keeps error-level noise out of the
    // test log so CI error parsers are not confused by it. FlushAll pushes the
    // line through the sink regardless of level.
    SPARK_LOG_INFO(Spark::LogCategory::Core, "%s", marker.c_str());
    Spark::Logger::Get().FlushAll();
    EXPECT_TRUE(ReadWholeFile(first).find(marker) != std::string::npos);

    const auto sizeAfterMarker = std::filesystem::file_size(first, pathError);
    ASSERT_TRUE(!pathError);

    // A second call (InitializeDebugSystemsImpl after an early-init entry point)
    // must not reopen or truncate the log file: same path, same installation, and
    // the bytes already written are still there.
    const std::string second = Spark::Core::Lifecycle::InstallEngineLogSinksImpl();
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, GetEngineRuntime().engineLogPath);
    EXPECT_TRUE(std::filesystem::exists(second, pathError));
    EXPECT_TRUE(std::filesystem::file_size(second, pathError) >= sizeAfterMarker);
    EXPECT_TRUE(ReadWholeFile(second).find(marker) != std::string::npos);
}

// ============================================================================
// core-01 / ai-anim-01 / core-09 / scripting-modding-ui-19 / mod-...-vs-02:
// engine-lifetime services are published on the context and withdrawn at shutdown
// ============================================================================

TEST(EngineWiring_LifecycleRegistersEngineLifetimeServices)
{
    SetupContextWithWorld();
    auto* ctx = EngineContext::Get();
    ASSERT_TRUE(ctx != nullptr);

    InitializeProductionLifecycle();

    EXPECT_TRUE(ctx->GetConditions() != nullptr);
    EXPECT_TRUE(ctx->GetAbilities() != nullptr);
    EXPECT_TRUE(ctx->GetInstances() != nullptr);
    EXPECT_TRUE(ctx->GetMusic() != nullptr);
    EXPECT_TRUE(ctx->GetDestruction() != nullptr);
    EXPECT_TRUE(ctx->GetWeapons() != nullptr);
    EXPECT_TRUE(ctx->GetWeapons() == GetEngineRuntime().weaponSystem.get());
    EXPECT_TRUE(ctx->GetAI() != nullptr);
    EXPECT_TRUE(ctx->GetAnimation() != nullptr);
    EXPECT_TRUE(ctx->GetTween() != nullptr);
    EXPECT_TRUE(ctx->GetVFS() != nullptr);
    EXPECT_TRUE(ctx->GetAreaStreaming() != nullptr);
    EXPECT_TRUE(ctx->GetLocalization() != nullptr);
    EXPECT_TRUE(ctx->GetCinematic() != nullptr);
    EXPECT_TRUE(ctx->GetReplay() != nullptr);
    EXPECT_TRUE(ctx->GetInvalidStateDetector() != nullptr);
    EXPECT_TRUE(ctx->GetComponentSerializers() != nullptr);

    // A module reaching these through IEngineContext must get the host instance,
    // not a copy: the registered pointers are the engine's own singletons.
    EXPECT_TRUE(ctx->GetLocalization() == &Spark::LocalizationSystem::Get());

    Spark::Core::Lifecycle::ShutdownGameplaySystemsImpl();

    EXPECT_TRUE(ctx->GetConditions() == nullptr);
    EXPECT_TRUE(ctx->GetAbilities() == nullptr);
    EXPECT_TRUE(ctx->GetMusic() == nullptr);
    EXPECT_TRUE(ctx->GetDestruction() == nullptr);
    EXPECT_TRUE(ctx->GetWeapons() == nullptr);
    EXPECT_TRUE(GetEngineRuntime().weaponSystem == nullptr);
    EXPECT_TRUE(ctx->GetAI() == nullptr);
    EXPECT_TRUE(ctx->GetAnimation() == nullptr);
    EXPECT_TRUE(ctx->GetTween() == nullptr);
    EXPECT_TRUE(ctx->GetVFS() == nullptr);
    EXPECT_TRUE(ctx->GetAreaStreaming() == nullptr);
    EXPECT_TRUE(ctx->GetLocalization() == nullptr);
    EXPECT_TRUE(ctx->GetComponentSerializers() == nullptr);
}

TEST(EngineWiring_MovieRenderPipelineRequiresExplicitOwnership)
{
    SetupContextWithWorld();
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Shutdown();

    InitializeProductionLifecycle();

    Spark::Rendering::MovieRenderSettings settings;
    settings.qualityPreset = Spark::Rendering::RenderQuality::Custom;
    settings.aaSamples = 1;
    settings.motionBlurSubFrames = 1;
    settings.startFrame = 0;
    settings.endFrame = 1;

    // Ordinary engine startup must not silently opt into an offline tool.
    EXPECT_FALSE(pipeline.StartRender(settings));

    pipeline.Initialize();
    ASSERT_TRUE(pipeline.StartRender(settings));
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(1.0f / 60.0f);
    EXPECT_EQ(pipeline.GetCurrentJob().currentFrame, 0);

    // The engine must not stop a job owned by an explicit caller either.
    Spark::Core::Lifecycle::ShutdownGameplaySystemsImpl();
    EXPECT_TRUE(pipeline.IsRendering());
    pipeline.Shutdown();
}

// ============================================================================
// engine-longtail-05: ProximityTriggerSystem::Update defers callbacks
// ============================================================================

TEST(EngineWiring_ProximityTriggerDefersCallbacksUntilScanCompletes)
{
    auto& triggers = Spark::World::ProximityTriggerSystem::GetInstance();
    triggers.Initialize();

    int enterCount = 0;
    uint32_t selfRemovingId = 0;
    // A one-shot style callback that removes its own trigger while the system is
    // dispatching: before deferred dispatch this erased from the map being iterated.
    selfRemovingId = triggers.CreateSphereTrigger(
        {0.0f, 0.0f, 0.0f}, 5.0f,
        [&enterCount](uint32_t triggerID, uint32_t)
        {
            ++enterCount;
            Spark::World::ProximityTriggerSystem::GetInstance().RemoveTrigger(triggerID);
        },
        nullptr);
    const uint32_t plainId = triggers.CreateSphereTrigger(
        {0.0f, 0.0f, 0.0f}, 5.0f, [&enterCount](uint32_t, uint32_t) { ++enterCount; }, nullptr);
    ASSERT_TRUE(selfRemovingId != 0);
    ASSERT_TRUE(plainId != 0);
    EXPECT_EQ(triggers.GetTotalTriggerCount(), 2u);

    std::vector<Spark::World::EntityPosition> positions;
    positions.push_back({42u, {1.0f, 0.0f, 0.0f}});
    triggers.Update(positions);

    EXPECT_EQ(enterCount, 2);
    EXPECT_EQ(triggers.GetTotalTriggerCount(), 1u);

    // Second scan: the remaining trigger already holds the entity, so no re-entry.
    triggers.Update(positions);
    EXPECT_EQ(enterCount, 2);

    triggers.Shutdown();
}

// ============================================================================
// core-03: TriggerVolumeComponent is bridged and ticked by the lifecycle
// ============================================================================

TEST(EngineWiring_TriggerVolumeComponentPublishesEnterEventFromLifecycleTick)
{
    World& world = SetupContextWithWorld();
    // Other tests can reach this static world through EngineContext. Retire
    // their entities so this trigger fixture starts identically in shuffled runs.
    const auto& registry = world.GetRegistry();
    std::vector<EntityID> staleEntities;
    if (const auto* entities = registry.storage<entt::entity>())
    {
        for (auto [entity] : entities->each())
            staleEntities.push_back(entity);
    }
    for (EntityID entity : staleEntities)
    {
        if (registry.valid(entity))
            world.DestroyEntity(entity);
    }
    InitializeProductionLifecycle();

    const EntityID volume = world.CreateEntity("wiring_trigger_volume");
    world.AddComponent<Transform>(volume);
    auto& tv = world.AddComponent<TriggerVolumeComponent>(volume);
    tv.shape = TriggerVolumeComponent::Shape::Sphere;
    tv.radius = 5.0f;

    const EntityID visitor = world.CreateEntity("wiring_trigger_visitor");
    auto& visitorTransform = world.AddComponent<Transform>(visitor);
    visitorTransform.position = {1.0f, 0.0f, 0.0f};

    std::vector<Spark::TriggerEnterEvent> received;
    auto subscription = ContextEventBus().Subscribe<Spark::TriggerEnterEvent>(
        [&received](const Spark::TriggerEnterEvent& e) { received.push_back(e); });

    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(1.0f / 60.0f);

    // The component was bound to a runtime trigger and the visitor's entry was
    // published on the engine EventBus with the volume entity as triggerId.
    EXPECT_TRUE(tv.runtimeTriggerID != 0);
    ASSERT_TRUE(!received.empty());
    EXPECT_EQ(received.front().entityId, static_cast<uint32_t>(visitor));
    EXPECT_EQ(received.front().triggerId, static_cast<uint32_t>(volume));

    // Still inside: no second enter event on the next tick.
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(1.0f / 60.0f);
    EXPECT_EQ(received.size(), static_cast<size_t>(1));

    Spark::World::ProximityTriggerSystem::GetInstance().RemoveTrigger(tv.runtimeTriggerID);
    world.DestroyEntity(visitor);
    world.DestroyEntity(volume);
}

// ============================================================================
// ENG-200: the lifecycle connects the script runtime to the engine EventBus, so
// physics contact events reach entity scripts with no game-module glue
// ============================================================================

#ifdef SPARK_ANGELSCRIPT_SUPPORT
TEST(EngineWiring_LifecycleConnectsScriptContactDispatch)
{
    World& world = SetupContextWithWorld();
    InitializeProductionLifecycle();
    auto* ctx = EngineContext::Get();
    AngelScriptEngine* scripts = ctx->GetScriptEngine();
    ASSERT_TRUE(scripts != nullptr);

    ASSERT_TRUE(
        scripts->CompileScriptFromString("class WiringContactProbe\n"
                                         "{\n"
                                         "    void OnCollision(EntityID other) { fireEvent(\"wired:\" + other); }\n"
                                         "}\n",
                                         "EngineWiringContactProbe"));
    // Entity 0 is the physics "no entity" id and never takes part in script contacts. Only the first entity a fresh
    // World creates has that id, so the filler absorbs it and both participants are real entities.
    const EntityID filler = world.CreateEntity("wiring_contact_filler");
    const EntityID probe = world.CreateEntity("wiring_contact_probe");
    const EntityID other = world.CreateEntity("wiring_contact_other");
    ASSERT_TRUE(scripts->AttachScript(probe, "WiringContactProbe", "EngineWiringContactProbe"));

    std::vector<Spark::ScriptEvent> received;
    auto subscription = ContextEventBus().Subscribe<Spark::ScriptEvent>([&received](const Spark::ScriptEvent& e)
                                                                        { received.push_back(e); });

    // Published exactly as PhysicsSystem publishes it after a step. Only the lifecycle's ConnectEventBus() call
    // routes it to the script: nothing in this test connects the script engine to the bus.
    ContextEventBus().Publish(Spark::CollisionEvent{static_cast<uint32_t>(probe), static_cast<uint32_t>(other), 0.0f});

    ASSERT_EQ(received.size(), static_cast<size_t>(1));
    EXPECT_EQ(received.front().sourceEntity, static_cast<uint32_t>(probe));
    EXPECT_EQ(received.front().eventName, "wired:" + std::to_string(static_cast<uint32_t>(other)));

    scripts->DetachScript(probe);
    world.DestroyEntity(probe);
    world.DestroyEntity(other);
    world.DestroyEntity(filler);
    Spark::Core::Lifecycle::ShutdownGameplaySystemsImpl();
}
#endif

// ============================================================================
// engine-longtail-03: the lifecycle produces RecordFrame while recording
// ============================================================================

TEST(EngineWiring_ReplayCaptureRecordsFramesWhileRecording)
{
    World& world = SetupContextWithWorld();
    InitializeProductionLifecycle();

    const EntityID mover = world.CreateEntity("wiring_replay_mover");
    auto& transform = world.AddComponent<Transform>(mover);
    transform.position = {2.0f, 0.0f, 0.0f};

    auto& replay = Spark::ReplaySystem::GetInstance();
    replay.SetRecordInterval(0.05f);
    replay.StartRecording();
    EXPECT_EQ(replay.GetFrameCount(), static_cast<size_t>(0));

    // Two ticks a record-interval apart: each must capture a frame.
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.1f);
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.1f);

    EXPECT_TRUE(replay.GetFrameCount() >= 2);
    EXPECT_TRUE(replay.GetDuration() > 0.0f);

    replay.StopRecording();
    EXPECT_FALSE(replay.IsRecording());
    world.DestroyEntity(mover);
}

// ============================================================================
// physics-audio-input-camera-06: InputActionSystem reads the registered InputManager
// ============================================================================

TEST(EngineWiring_InputActionProvidersReadTheRegisteredInputManager)
{
    SetupContextWithWorld();
    auto* ctx = EngineContext::Get();
    ASSERT_TRUE(ctx != nullptr);

    {
        InputManager input; // No window: key state is a plain map, exactly what the providers read.
        ctx->SetInput(&input);
        InitializeProductionLifecycle();

        auto& actions = Spark::Input::InputActionSystem::GetInstance();
        ASSERT_TRUE(actions.RegisterAction("WiringProbe", Spark::Input::ActionType::Button));
        ASSERT_TRUE(actions.BindKey("WiringProbe", 'A', Spark::Input::InputTrigger::Held));

        // Hold the key for a moment, then run the production tick: without the
        // providers bound by the lifecycle the action can never become active.
        input.Console_SimulateKeyPress("A", 250);
        EXPECT_TRUE(input.IsKeyDown('A'));
        actions.Update();
        EXPECT_TRUE(actions.IsActionActive("WiringProbe"));

        // Drop the providers before the InputManager they capture goes away.
        actions.SetKeyStateProviders({}, {}, {});
        ctx->SetInput(nullptr);
    }
}

// A timed console key press used to be released by a detached timer thread that
// wrote m_keyStates while the frame code read it without a lock (ThreadSanitizer
// reported the race from the test above on every Linux run). The release is now
// queued and applied on the input thread by Update() / ApplyDueTimedKeyReleases().
TEST(EngineWiring_ConsoleSimulatedKeyReleaseIsAppliedOnTheInputThread)
{
    InputManager input; // No window: the key state is a plain map.

    // A press whose deadline cannot be reached during this test stays down
    // across an apply call (nothing waits on it; ClearInputStates drops it).
    input.Console_SimulateKeyPress("A", 600000);
    EXPECT_TRUE(input.IsKeyDown('A'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 1u);
    input.ApplyDueTimedKeyReleases();
    EXPECT_TRUE(input.IsKeyDown('A'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 1u);

    // A press whose deadline has passed is released by the next apply call, on
    // this thread, and only once the caller asks: nothing happens in between.
    input.Console_SimulateKeyPress("B", 20);
    EXPECT_TRUE(input.IsKeyDown('B'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_TRUE(input.IsKeyDown('B'));
    input.ApplyDueTimedKeyReleases();
    EXPECT_FALSE(input.IsKeyDown('B'));
    EXPECT_TRUE(input.IsKeyDown('A'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 1u);

    // The frame tick drains due releases too. Edges are latched per Update(), so
    // a frame must observe C down before a later frame can see it released. The
    // deadline is generous so a loaded host cannot release C before the first
    // frame; frames then tick until the release lands, bounded well past it.
    constexpr auto releaseAfter = std::chrono::milliseconds(1000);
    const auto pressedAt = std::chrono::steady_clock::now();
    input.Console_SimulateKeyPress("C", static_cast<int>(releaseAfter.count()));
    input.Update();
    ASSERT_TRUE(input.IsKeyDown('C'));
    EXPECT_TRUE(input.WasKeyPressed('C'));
    const auto giveUpAt = pressedAt + releaseAfter + std::chrono::seconds(30);
    while (input.IsKeyDown('C') && std::chrono::steady_clock::now() < giveUpAt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        input.Update();
        if (input.IsKeyDown('C'))
            EXPECT_FALSE(input.WasKeyPressed('C')); // held, not re-pressed, until the release frame
    }
    EXPECT_FALSE(input.IsKeyDown('C'));
    EXPECT_TRUE(input.WasKeyReleased('C'));
    // Released by the tick no earlier than its deadline.
    EXPECT_TRUE(std::chrono::steady_clock::now() - pressedAt >= releaseAfter);
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 1u);

    // Clearing the input states also forgets the pending timed releases.
    input.Console_ClearInputStates();
    EXPECT_FALSE(input.IsKeyDown('A'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 0u);

    // A zero-duration press is a complete press/release on the calling thread.
    input.Console_SimulateKeyPress("D", 0);
    EXPECT_FALSE(input.IsKeyDown('D'));
    EXPECT_EQ(input.GetPendingTimedKeyReleaseCount(), 0u);
}

// Uncaptured mouse motion delivered by the pump before the tick is that frame's
// delta. The Windows Update() used to advance the previous position before
// measuring against it, so every uncaptured delta there was zero.
TEST(EngineWiring_UncapturedMouseMoveIsTheFrameDelta)
{
    InputManager input; // No window and no capture: WM_MOUSEMOVE carries client coordinates.
    const auto mouseMove = [&input](int x, int y)
    {
        const uint32_t packed = (static_cast<uint32_t>(y) << 16) | static_cast<uint32_t>(x);
        input.HandleMessage(WM_MOUSEMOVE, 0, static_cast<LPARAM>(packed));
    };

    input.Update(); // an idle frame at the origin
    mouseMove(100, 40);
    input.Update();
    EXPECT_EQ(input.GetMousePosition().x, 100);
    EXPECT_EQ(input.GetMousePosition().y, 40);
    EXPECT_EQ(input.GetMouseDelta().x, 100);
    EXPECT_EQ(input.GetMouseDelta().y, 40);

    // Several moves in one frame add up to one delta from the previous frame.
    mouseMove(110, 30);
    mouseMove(130, 25);
    input.Update();
    EXPECT_EQ(input.GetMouseDelta().x, 30);
    EXPECT_EQ(input.GetMouseDelta().y, -15);

    // A frame without motion has no delta.
    input.Update();
    EXPECT_EQ(input.GetMouseDelta().x, 0);
    EXPECT_EQ(input.GetMouseDelta().y, 0);
}

// ============================================================================
// Module teardown: registries a game module fills with objects built in its own
// image must be emptied by gameplay teardown, which runs after module OnUnload
// and before FreeLibrary. Left to the singletons' process-exit destructors, the
// installed ARPG and RPG packages crashed with an access violation after their
// DLL was unmapped (behavior-tree node vtables, a make_shared clip control block).
// ============================================================================

namespace
{
    // Static, not a test-frame local: when the release regresses, the nodes are
    // destroyed at process exit and must not write into a dead stack frame.
    int s_destroyedTeardownProbeNodes = 0;

    class TeardownProbeNode final : public Spark::AI::BTNode
    {
      public:
        ~TeardownProbeNode() override { ++s_destroyedTeardownProbeNodes; }

        Spark::AI::NodeStatus Tick(float, Spark::AI::Blackboard&) override
        {
            return m_status = Spark::AI::NodeStatus::Success;
        }

        std::unique_ptr<Spark::AI::BTNode> Clone() const override { return std::make_unique<TeardownProbeNode>(); }
    };
} // namespace

TEST(EngineWiring_GameplayTeardownReleasesModuleBuiltRegistryEntries)
{
    SetupContextWithWorld();
    auto* ctx = EngineContext::Get();
    ASSERT_TRUE(ctx != nullptr);

    InitializeProductionLifecycle();
    ASSERT_TRUE(ctx->GetAI() != nullptr);
    ASSERT_TRUE(ctx->GetAnimation() != nullptr);
    ASSERT_TRUE(ctx->GetDestruction() != nullptr);

    // What SparkGameARPG/SparkGameRPG register through IEngineContext at OnLoad:
    // a behavior template (plus the per-agent clone the AI tick makes from it)...
    s_destroyedTeardownProbeNodes = 0;
    auto tree = std::make_unique<Spark::AI::BehaviorTree>("teardown_probe");
    tree->SetRoot(std::make_unique<TeardownProbeNode>());
    ctx->GetAI()->RegisterBehavior("teardown_probe", std::move(tree));
    ASSERT_TRUE(ctx->GetAI()->CreateBehaviorInstance("teardown_probe") != nullptr);

    // ...an animation clip...
    auto clip = std::make_shared<Spark::Animation::AnimationClip>();
    const std::weak_ptr<Spark::Animation::AnimationClip> clipWatch = clip;
    ctx->GetAnimation()->RegisterClip("teardown_probe", std::move(clip));

    // ...and a destruction callback.
    auto callbackState = std::make_shared<int>(0);
    const std::weak_ptr<int> callbackWatch = callbackState;
    ctx->GetDestruction()->OnDestruction([callbackState](const Spark::DestructionEvent&) { ++*callbackState; });
    callbackState.reset();

    EXPECT_EQ(s_destroyedTeardownProbeNodes, 0);
    EXPECT_FALSE(clipWatch.expired());
    EXPECT_FALSE(callbackWatch.expired());

    Spark::Core::Lifecycle::ShutdownGameplaySystemsImpl();

    EXPECT_EQ(s_destroyedTeardownProbeNodes, 2); // the template root and its clone
    EXPECT_TRUE(clipWatch.expired());
    EXPECT_TRUE(callbackWatch.expired());
}
