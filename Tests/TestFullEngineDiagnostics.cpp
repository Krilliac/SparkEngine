/**
 * @file TestFullEngineDiagnostics.cpp
 * @brief Comprehensive engine test — initializes ALL subsystems and runs
 *        the complete diagnostic suite (core + extended).
 *
 * This is the "start the whole engine and test everything" test.
 * It initializes: EngineContext, EventBus, World, Physics, Weather, TimeOfDay,
 * Coroutines, Abilities, Conditions, Instances, Dialogue, Tweens, SaveSystem,
 * VFS, FileCache, Networking, NullRHI, JobSystem, AI systems, Debug systems,
 * Rendering systems, Streaming, Destruction, Freeze, and Scripting.
 */

#include "TestFramework.h"
#include "Core/EngineDiagnostics.h"
#include "Core/EngineContext.h"

// Core systems
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/Gameplay/AbilitySystem.h"
#include "Engine/Gameplay/ConditionSystem.h"
#include "Engine/Gameplay/InstanceManager.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Tween/TweenSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Graphics/WeatherSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/EventBus.h"
#include "Utils/LocalFileCache.h"
#include "Utils/MemoryMonitor.h"

// Extended systems
#include "Engine/AI/CollisionAvoidance.h"
#include "Engine/AI/CoverSystem.h"
#include "Engine/AI/FormationSystem.h"
#include "Engine/AI/GroupAI.h"
#include "Engine/AI/TacticalPointSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/SaveSystem/FreezeSystem.h"
#include "Engine/Streaming/SeamlessAreaManager.h"
#include "Engine/World/ProximityTriggerSystem.h"
#include "Graphics/ClusteredLightCulling.h"
#include "Graphics/RHI/NullRHIDevice.h"
#include "Utils/ChromeTracing.h"
#include "Utils/DebugHookManager.h"
#include "Utils/FrameInspector.h"
#include "Utils/GPUPerfCounters.h"
#include "Utils/JobSystem.h"
#include "Utils/Profiler.h"
#include "Utils/Timer.h"
#include "Core/FixedTimestepAccumulator.h"

#include <chrono>
#include <iostream>

// ============================================================================
// Full engine bootstrap — initializes EVERY reachable subsystem
// ============================================================================

static bool g_fullEngineInitialized = false;

static void InitFullEngine()
{
    if (g_fullEngineInitialized)
        return;

    // --- EngineContext ---
    if (!EngineContext::Get())
        EngineContext::SetOwned(std::make_unique<EngineContext>());
    auto* ctx = EngineContext::Get();

    // --- Core infrastructure ---
    static Spark::EventBus eventBus;
    ctx->SetEventBus(&eventBus);

    static World world;
    ctx->SetWorld(&world);

    // --- Physics (stub mode if Jolt unavailable) ---
    // PhysicsSystem must be heap-allocated and explicitly shutdown before the
    // SimpleConsole singleton is destroyed (PhysicsSystem::~PhysicsSystem logs).
    static std::unique_ptr<PhysicsSystem> physics;
    if (!physics)
    {
        physics = std::make_unique<PhysicsSystem>();
        physics->Initialize();
        std::atexit(
            []()
            {
                if (physics)
                {
                    physics->Shutdown();
                    physics.reset();
                }
            });
    }
    ctx->SetPhysics(physics.get());

    // --- Weather ---
    static Spark::WeatherSystem weather;
    ctx->SetWeather(&weather);

    // --- TimeOfDay ---
    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    // --- Coroutines ---
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    // --- Abilities ---
    auto& abilities = Spark::Gameplay::AbilitySystem::GetInstance();
    abilities.Initialize(&eventBus);
    ctx->SetAbilities(&abilities);

    // --- Conditions ---
    auto& conditions = Spark::Gameplay::ConditionSystem::GetInstance();
    conditions.Initialize();
    ctx->SetConditions(&conditions);

    // --- Instances ---
    auto& instances = Spark::Gameplay::InstanceManager::GetInstance();
    instances.Initialize();
    ctx->SetInstances(&instances);

    // --- Dialogue ---
    static Spark::DialogueSystem dialogue;
    ctx->SetDialogue(&dialogue);

    // --- Tweens ---
    auto& tweens = Spark::TweenSystem::GetInstance();
    tweens.Initialize();
    ctx->SetTween(&tweens);

    // --- SaveSystem ---
    auto& save = Spark::SaveSystem::GetInstance();
    save.Initialize();
    ctx->SetSaveSystem(&save);

    // --- VFS ---
    auto& vfs = Spark::VirtualFileSystem::GetInstance();
    vfs.Initialize();
    ctx->SetVFS(&vfs);

    // --- FileCache ---
    static Spark::LocalFileCache fileCache;
    ctx->SetFileCache(&fileCache);

    // --- Destruction ---
    auto& destruction = Spark::DestructionSystem::GetInstance();
    destruction.Initialize();
    ctx->SetDestruction(&destruction);

    // --- Streaming ---
    auto& streaming = Spark::Streaming::SeamlessAreaManager::GetInstance();
    streaming.Initialize();
    ctx->SetAreaStreaming(&streaming);

    // --- NullRHI (headless graphics) ---
    auto& nullRHI = Spark::RHI::NullRHIDevice::GetInstance();
    if (!nullRHI.IsInitialized())
    {
        Spark::RHI::RHIDeviceDesc desc;
        nullRHI.Initialize(desc);
    }

    // --- JobSystem ---
    auto& jobs = Spark::JobSystem::Get();
    if (!jobs.IsInitialized())
        jobs.Initialize(2);

    // --- AI Systems ---
    Spark::AI::TacticalPointSystem::GetInstance().Initialize();
    Spark::AI::CoverSystem::GetInstance().Initialize();
    Spark::AI::FormationSystem::GetInstance().Initialize();
    Spark::AI::GroupAISystem::GetInstance().Initialize();
    Spark::AI::CollisionAvoidanceSystem::GetInstance().Initialize();
    Spark::World::ProximityTriggerSystem::GetInstance().Initialize();

    // --- Debug Systems ---
    Spark::DebugHookManager::GetInstance().SetEnabled(true);
    Spark::MemoryMonitor::GetInstance().Initialize();
    Profiler::GetInstance().SetEnabled(true);

    // --- Rendering Utilities ---
    Spark::Graphics::ClusteredLightCulling::GetInstance().Initialize();
    Spark::Graphics::GPUPerfCounters::GetInstance();

    // --- Freeze ---
    Spark::FreezeSystem::GetInstance().Initialize();

    // --- FixedTimestep ---
    Spark::FixedTimestepAccumulator::GetInstance().Initialize();

    g_fullEngineInitialized = true;
}

// ============================================================================
// TEST: Full engine DiagRunAll
// ============================================================================

TEST(FullEngine_DiagRunAll)
{
    InitFullEngine();

    Spark::DiagReport report;
    Spark::DiagRunAll(report);

    std::string output = report.Format();
    std::cout << "\n" << output << std::flush;

    // Count results by subsystem
    std::cout << "\n=== Per-Subsystem Summary ===\n";
    std::string lastSub;
    int subPassed = 0, subFailed = 0;
    for (const auto& r : report.results)
    {
        if (r.subsystem != lastSub)
        {
            if (!lastSub.empty())
            {
                std::cout << "  " << lastSub << ": " << subPassed << " passed, " << subFailed << " failed\n";
            }
            lastSub = r.subsystem;
            subPassed = 0;
            subFailed = 0;
        }
        if (r.passed)
            subPassed++;
        else
            subFailed++;
    }
    if (!lastSub.empty())
        std::cout << "  " << lastSub << ": " << subPassed << " passed, " << subFailed << " failed\n";
    std::cout << std::flush;

    // Assertions
    EXPECT_TRUE(report.results.size() > 100);
    EXPECT_TRUE(report.passed > 100);
    EXPECT_TRUE(report.failed <= 2); // allow physics body ops if stub
}

// ============================================================================
// TEST: Full engine frame simulation (200 frames with all systems)
// ============================================================================

TEST(FullEngine_SimulateFrames)
{
    InitFullEngine();
    auto* ctx = EngineContext::Get();

    constexpr int NUM_FRAMES = 200;
    constexpr float DT = 1.0f / 60.0f;

    auto wallStart = std::chrono::high_resolution_clock::now();

    for (int frame = 0; frame < NUM_FRAMES; frame++)
    {
        // Tick all systems as the engine would
        Spark::FixedTimestepAccumulator::GetInstance().Advance(DT);

        if (auto* w = ctx->GetWeather())
            w->Update(DT);
        if (auto* tod = ctx->GetTimeOfDay())
            tod->Update(DT);
        if (auto* coro = ctx->GetCoroutineScheduler())
            coro->Update(DT);
        if (auto* tw = ctx->GetTween())
            tw->Update(DT);
        if (auto* ab = ctx->GetAbilities())
        {
            if (ctx->GetWorld())
                ab->Update(*ctx->GetWorld(), DT);
        }
        if (auto* inst = ctx->GetInstances())
            inst->Update(DT);

        // Physics
        if (auto* phys = ctx->GetPhysics())
            phys->Update(DT);

        // Debug frame cycle
        Profiler::GetInstance().BeginFrame();
        Profiler::GetInstance().EndFrame();

        auto& inspector = Spark::FrameInspector::GetInstance();
        inspector.OnFrameEnd();

        // GPU counters
        auto& gpuC = Spark::Graphics::GPUPerfCounters::GetInstance();
        gpuC.Increment(Spark::Graphics::GPUCounterCategory::DrawCalls, 10);
        gpuC.EndFrame();

        // NullRHI frame
        auto& rhi = Spark::RHI::NullRHIDevice::GetInstance();
        rhi.BeginFrame();
        rhi.EndFrame();
    }

    auto wallEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(wallEnd - wallStart).count();
    double perFrameMs = totalMs / NUM_FRAMES;

    std::cout << "\n=== Full Engine Frame Simulation (200 frames) ===\n";
    std::cout << "  Total wall time: " << std::fixed << std::setprecision(1) << totalMs << " ms\n";
    std::cout << "  Per frame:       " << std::setprecision(3) << perFrameMs << " ms\n";
    std::cout << "  Effective FPS:   " << std::setprecision(0) << (1000.0 / perFrameMs) << "\n";
    std::cout << std::flush;

    EXPECT_TRUE(perFrameMs < 10.0); // should be fast in headless
}

// ============================================================================
// TEST: Entity lifecycle with all components under full engine
// ============================================================================

TEST(FullEngine_EntityLifecycle)
{
    InitFullEngine();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    size_t baseline = world->GetEntityCount();

    // Create entities with various component combinations
    auto e1 = world->CreateEntity("__full_test_player");
    world->AddComponent<Transform>(e1, Transform{});
    world->AddComponent<HealthComponent>(e1, HealthComponent{100.0f, 100.0f});

    auto e2 = world->CreateEntity("__full_test_enemy");
    world->AddComponent<Transform>(e2, Transform{});
    world->AddComponent<HealthComponent>(e2, HealthComponent{50.0f, 50.0f});
    world->AddComponent<AIComponent>(e2, AIComponent{});

    auto e3 = world->CreateEntity("__full_test_light");
    world->AddComponent<Transform>(e3, Transform{});
    world->AddComponent<LightComponent>(e3, LightComponent{});

    EXPECT_EQ(world->GetEntityCount(), baseline + 3);

    // Query components
    auto* hp1 = world->GetComponent<HealthComponent>(e1);
    EXPECT_TRUE(hp1 != nullptr && hp1->maxHealth == 100.0f);

    auto* ai = world->GetComponent<AIComponent>(e2);
    EXPECT_TRUE(ai != nullptr);

    auto* light = world->GetComponent<LightComponent>(e3);
    EXPECT_TRUE(light != nullptr);

    // Modify and verify
    if (hp1)
        hp1->health = 75.0f;
    auto* hp1Check = world->GetComponent<HealthComponent>(e1);
    EXPECT_TRUE(hp1Check && hp1Check->health == 75.0f);

    // Remove component
    world->RemoveComponent<AIComponent>(e2);
    EXPECT_TRUE(!world->HasComponent<AIComponent>(e2));
    EXPECT_TRUE(world->HasComponent<HealthComponent>(e2));

    // Destroy all
    world->DestroyEntity(e1);
    world->DestroyEntity(e2);
    world->DestroyEntity(e3);
    EXPECT_EQ(world->GetEntityCount(), baseline);
}

// ============================================================================
// TEST: JobSystem stress under full engine
// ============================================================================

TEST(FullEngine_JobSystemStress)
{
    InitFullEngine();
    auto& jobs = Spark::JobSystem::Get();

    // Submit 500 jobs that do real work
    constexpr int N = 500;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; i++)
    {
        futures.push_back(jobs.Submit(
            [&counter]()
            {
                // Simulate work
                volatile int sum = 0;
                for (int j = 0; j < 100; j++)
                    sum += j;
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
    }

    for (auto& f : futures)
        f.get();

    EXPECT_EQ(counter.load(), N);

    // ParallelFor with large range
    std::vector<int> results(10000, 0);
    jobs.ParallelFor(0, 10000, [&](int i) { results[i] = i + 1; });

    int sum = 0;
    for (int v : results)
        sum += v;
    // sum of 1..10000 = 50005000
    EXPECT_EQ(sum, 50005000);

    std::cout << "\n=== JobSystem Stress ===\n";
    std::cout << "  500 async jobs completed: " << counter.load() << "/500\n";
    std::cout << "  ParallelFor 10k items sum: " << sum << " (expected 50005000)\n" << std::flush;
}

// ============================================================================
// TEST: Network + Physics + Weather combined simulation
// ============================================================================

TEST(FullEngine_CombinedSimulation)
{
    InitFullEngine();
    auto* ctx = EngineContext::Get();

    // Start a network server
    auto& net = Spark::Net::NetworkManager::GetInstance();
    bool serverStarted = net.StartServer(58888, 8);

    // Set weather to storm
    auto* weather = ctx->GetWeather();
    if (weather)
        weather->SetWeather(Spark::WeatherType::Storm, 1.0f, 0.0f);

    // Set time to noon
    auto* tod = ctx->GetTimeOfDay();
    if (tod)
        tod->SetTimeOfDay(12.0f);

    // Run 60 frames of combined simulation
    constexpr float DT = 1.0f / 60.0f;
    for (int i = 0; i < 60; i++)
    {
        if (weather)
            weather->Update(DT);
        if (tod)
            tod->Update(DT);
        if (auto* phys = ctx->GetPhysics())
            phys->Update(DT);
        if (serverStarted)
            net.Update(DT);
    }

    // Verify states
    if (weather)
    {
        auto state = weather->GetCurrentState();
        EXPECT_TRUE(state.type == Spark::WeatherType::Storm);
        EXPECT_TRUE(state.intensity > 0.9f);
    }

    if (tod)
    {
        float sunIntensity = tod->GetSunIntensity();
        EXPECT_TRUE(sunIntensity > 0.5f); // noon = bright
    }

    if (serverStarted)
    {
        EXPECT_TRUE(net.GetRole() == Spark::Net::NetworkRole::Server);
        net.StopServer();
    }

    // Restore
    if (weather)
        weather->SetWeather(Spark::WeatherType::Clear, 1.0f, 0.0f);

    std::cout << "\n=== Combined Simulation (Physics+Network+Weather+TimeOfDay, 60 frames) ===\n";
    std::cout << "  Network server: " << (serverStarted ? "OK" : "SKIP") << "\n";
    std::cout << "  Weather storm: OK\n";
    std::cout << "  TimeOfDay noon: OK\n" << std::flush;
}

// ============================================================================
// TEST: Debug system integration
// ============================================================================

TEST(FullEngine_DebugIntegration)
{
    InitFullEngine();

    // Chrome tracing roundtrip
    auto& tracing = Spark::ChromeTracing::GetInstance();
    tracing.Start();

    for (int i = 0; i < 10; i++)
    {
        tracing.BeginEvent("frame", "Engine");
        tracing.BeginEvent("physics", "Physics");
        tracing.EndEvent("physics", "Physics");
        tracing.BeginEvent("render", "Graphics");
        tracing.EndEvent("render", "Graphics");
        tracing.EndEvent("frame", "Engine");
    }

    size_t eventCount = tracing.EventCount();
    tracing.Stop();

    std::cout << "\n=== Debug Integration ===\n";
    std::cout << "  Chrome trace events: " << eventCount << "\n";

    EXPECT_TRUE(eventCount >= 40); // 10 frames x 4 events each

    // Profiler sections
    auto& profiler = Profiler::GetInstance();
    profiler.BeginFrame();
    profiler.BeginSection("test_section", ProfileCategory::Custom);
    profiler.EndSection("test_section");
    profiler.EndFrame();

    float sectionTime = profiler.GetSectionTimeMs("test_section");
    std::cout << "  Profiler section time: " << sectionTime << " ms\n";
    EXPECT_TRUE(sectionTime >= 0.0f);

    // Frame inspector
    auto& inspector = Spark::FrameInspector::GetInstance();
    inspector.CaptureSnapshot(0.016f, 1.0f);
    inspector.OnFrameEnd();

    std::cout << "  FrameInspector frame: " << inspector.GetFrameNumber() << "\n" << std::flush;

    tracing.Clear();
}

// ============================================================================
// TEST: RHI resource lifecycle
// ============================================================================

TEST(FullEngine_RHIResourceLifecycle)
{
    InitFullEngine();

    auto& device = Spark::RHI::NullRHIDevice::GetInstance();
    device.ResetNullStats();

    // Create a batch of resources
    constexpr int BATCH = 50;
    for (int i = 0; i < BATCH; i++)
    {
        auto b = device.CreateBuffer({});
        auto t = device.CreateTexture({});
    }

    // Run frame cycles
    for (int i = 0; i < 10; i++)
    {
        device.BeginFrame();
        device.EndFrame();
    }

    auto stats = device.GetNullStats();

    std::cout << "\n=== RHI Resource Lifecycle ===\n";
    std::cout << "  Buffers created:  " << stats.buffersCreated << "\n";
    std::cout << "  Textures created: " << stats.texturesCreated << "\n";
    std::cout << "  Frames rendered:  " << stats.framesRendered << "\n" << std::flush;

    EXPECT_EQ(stats.buffersCreated, static_cast<uint32_t>(BATCH));
    EXPECT_EQ(stats.texturesCreated, static_cast<uint32_t>(BATCH));
    EXPECT_EQ(stats.framesRendered, 10u);
}
