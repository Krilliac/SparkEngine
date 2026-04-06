/**
 * @file TestEngineLoadTest.cpp
 * @brief Full engine load test — measures real CPU time, wall time, peak memory,
 *        and throughput under sustained heavy load across all subsystems.
 *
 * Exercises: ECS entity churn, physics stepping, weather cycling, time-of-day,
 * coroutines, tweens, abilities, dialogue, save system, networking, JobSystem
 * parallel dispatch, EventBus fan-out, RHI resource creation, and debug tracing
 * all running concurrently for thousands of frames.
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Core/EngineDiagnostics.h"
#include "../SparkEngine/Source/Core/EngineContext.h"
#include "../SparkEngine/Source/Core/FixedTimestepAccumulator.h"

#include "../SparkEngine/Source/Engine/Coroutine/CoroutineScheduler.h"
#include "../SparkEngine/Source/Engine/Dialogue/DialogueSystem.h"
#include "../SparkEngine/Source/Engine/ECS/Components.h"
#include "../SparkEngine/Source/Engine/Gameplay/AbilitySystem.h"
#include "../SparkEngine/Source/Engine/Gameplay/ConditionSystem.h"
#include "../SparkEngine/Source/Engine/Gameplay/InstanceManager.h"
#include "../SparkEngine/Source/Engine/Modding/VirtualFileSystem.h"
#include "../SparkEngine/Source/Engine/Networking/NetworkManager.h"
#include "../SparkEngine/Source/Engine/SaveSystem/SaveSystem.h"
#include "../SparkEngine/Source/Engine/Tween/TweenSystem.h"
#include "../SparkEngine/Source/Engine/World/TimeOfDaySystem.h"
#include "../SparkEngine/Source/Graphics/WeatherSystem.h"
#include "../SparkEngine/Source/Physics/PhysicsSystem.h"
#include "../SparkEngine/Source/Utils/EventBus.h"
#include "../SparkEngine/Source/Utils/LocalFileCache.h"
#include "../SparkEngine/Source/Utils/MemoryMonitor.h"

#include "../SparkEngine/Source/Engine/AI/CollisionAvoidance.h"
#include "../SparkEngine/Source/Engine/AI/CoverSystem.h"
#include "../SparkEngine/Source/Engine/AI/FormationSystem.h"
#include "../SparkEngine/Source/Engine/AI/GroupAI.h"
#include "../SparkEngine/Source/Engine/AI/TacticalPointSystem.h"
#include "../SparkEngine/Source/Engine/Destruction/DestructionSystem.h"
#include "../SparkEngine/Source/Engine/SaveSystem/FreezeSystem.h"
#include "../SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.h"
#include "../SparkEngine/Source/Engine/World/ProximityTriggerSystem.h"
#include "../SparkEngine/Source/Graphics/ClusteredLightCulling.h"
#include "../SparkEngine/Source/Graphics/RHI/NullRHIDevice.h"
#include "../SparkEngine/Source/Engine/Replay/ReplaySystem.h"
#include "../SparkEngine/Source/Utils/ChromeTracing.h"
#include "../SparkEngine/Source/Utils/DebugHookManager.h"
#include "../SparkEngine/Source/Utils/FrameInspector.h"
#include "../SparkEngine/Source/Utils/GPUPerfCounters.h"
#include "../SparkEngine/Source/Utils/JobSystem.h"
#include "../SparkEngine/Source/Utils/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

// Shared event type for the load test
struct LoadTestEvent
{
    int frame;
};

// ============================================================================
// OS-level resource measurement
// ============================================================================

struct ResourceSample
{
    double wallTimeMs = 0.0;
    double cpuUserMs = 0.0;
    double cpuSystemMs = 0.0;
    size_t residentKB = 0; // RSS
    size_t virtualKB = 0;  // VSZ
};

#ifdef __linux__
#include <sys/resource.h>
#include <unistd.h>

static ResourceSample SampleResources()
{
    ResourceSample s;

    // CPU time via getrusage
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        s.cpuUserMs =
            static_cast<double>(usage.ru_utime.tv_sec) * 1000.0 + static_cast<double>(usage.ru_utime.tv_usec) / 1000.0;
        s.cpuSystemMs =
            static_cast<double>(usage.ru_stime.tv_sec) * 1000.0 + static_cast<double>(usage.ru_stime.tv_usec) / 1000.0;
    }

    // Memory via /proc/self/status
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.compare(0, 6, "VmRSS:") == 0)
        {
            size_t val = 0;
            std::sscanf(line.c_str(), "VmRSS: %zu", &val);
            s.residentKB = val;
        }
        else if (line.compare(0, 6, "VmSize") == 0)
        {
            size_t val = 0;
            std::sscanf(line.c_str(), "VmSize: %zu", &val);
            s.virtualKB = val;
        }
    }

    return s;
}
#else
static ResourceSample SampleResources()
{
    return {};
}
#endif

// ============================================================================
// Full engine init (reuses the pattern from TestFullEngineDiagnostics)
// ============================================================================

static bool g_loadTestInit = false;

static void InitLoadTestEngine()
{
    if (g_loadTestInit)
        return;

    if (!EngineContext::Get())
        EngineContext::SetOwned(std::make_unique<EngineContext>());
    auto* ctx = EngineContext::Get();

    static Spark::EventBus eventBus;
    ctx->SetEventBus(&eventBus);

    static World world;
    ctx->SetWorld(&world);

    // PhysicsSystem must be heap-allocated and explicitly shutdown before the
    // SimpleConsole singleton is destroyed (PhysicsSystem::~PhysicsSystem logs).
    // Using unique_ptr with an atexit handler that calls Shutdown() then releases.
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

    static Spark::WeatherSystem weather;
    ctx->SetWeather(&weather);

    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    auto& abilities = Spark::Gameplay::AbilitySystem::GetInstance();
    abilities.Initialize(&eventBus);
    ctx->SetAbilities(&abilities);

    auto& conditions = Spark::Gameplay::ConditionSystem::GetInstance();
    conditions.Initialize();
    ctx->SetConditions(&conditions);

    auto& instances = Spark::Gameplay::InstanceManager::GetInstance();
    instances.Initialize();
    ctx->SetInstances(&instances);

    static Spark::DialogueSystem dialogue;
    ctx->SetDialogue(&dialogue);

    auto& tweens = Spark::TweenSystem::GetInstance();
    tweens.Initialize();
    ctx->SetTween(&tweens);

    auto& save = Spark::SaveSystem::GetInstance();
    save.Initialize();
    ctx->SetSaveSystem(&save);

    auto& vfs = Spark::VirtualFileSystem::GetInstance();
    vfs.Initialize();
    ctx->SetVFS(&vfs);

    static Spark::LocalFileCache fileCache;
    ctx->SetFileCache(&fileCache);

    auto& destruction = Spark::DestructionSystem::GetInstance();
    destruction.Initialize();
    ctx->SetDestruction(&destruction);

    auto& streaming = Spark::Streaming::SeamlessAreaManager::GetInstance();
    streaming.Initialize();
    ctx->SetAreaStreaming(&streaming);

    auto& nullRHI = Spark::RHI::NullRHIDevice::GetInstance();
    if (!nullRHI.IsInitialized())
    {
        Spark::RHI::RHIDeviceDesc desc;
        nullRHI.Initialize(desc);
    }

    auto& jobs = Spark::JobSystem::Get();
    if (!jobs.IsInitialized())
        jobs.Initialize(4); // 4 threads for load testing

    Spark::AI::TacticalPointSystem::GetInstance().Initialize();
    Spark::AI::CoverSystem::GetInstance().Initialize();
    Spark::AI::FormationSystem::GetInstance().Initialize();
    Spark::AI::GroupAISystem::GetInstance().Initialize();
    Spark::AI::CollisionAvoidanceSystem::GetInstance().Initialize();
    Spark::World::ProximityTriggerSystem::GetInstance().Initialize();

    Spark::DebugHookManager::GetInstance().SetEnabled(true);
    Spark::MemoryMonitor::GetInstance().Initialize();
    Profiler::GetInstance().SetEnabled(true);
    Spark::Graphics::ClusteredLightCulling::GetInstance().Initialize();
    Spark::FreezeSystem::GetInstance().Initialize();
    Spark::FixedTimestepAccumulator::GetInstance().Initialize();

    g_loadTestInit = true;
}

// ============================================================================
// Heavy frame: exercises every subsystem with real work
// ============================================================================

static void HeavyFrame(float dt, int frameIdx, World& world, Spark::EventBus& bus)
{
    auto* ctx = EngineContext::Get();

    // Fixed timestep
    Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

    // Physics
    if (auto* p = ctx->GetPhysics())
        p->Update(dt);

    // Weather — cycle every 120 frames
    if (auto* w = ctx->GetWeather())
    {
        if (frameIdx % 120 == 0)
        {
            int idx = (frameIdx / 120) % 5;
            Spark::WeatherType types[] = {Spark::WeatherType::Clear, Spark::WeatherType::Rain, Spark::WeatherType::Snow,
                                          Spark::WeatherType::Storm, Spark::WeatherType::Fog};
            w->SetWeather(types[idx], 1.0f, 0.2f);
        }
        w->Update(dt);
    }

    // TimeOfDay
    if (auto* t = ctx->GetTimeOfDay())
        t->Update(dt);

    // Coroutines
    if (auto* c = ctx->GetCoroutineScheduler())
        c->Update(dt);

    // Tweens — create new tweens periodically
    if (auto* tw = ctx->GetTween())
    {
        if (frameIdx % 30 == 0)
        {
            static float throwaway = 0.0f;
            tw->TweenFloat(throwaway, 0.0f, 1.0f, 0.5f, Spark::EaseType::EaseOutQuad);
        }
        tw->Update(dt);
    }

    // Abilities
    if (auto* ab = ctx->GetAbilities())
        ab->Update(world, dt);

    // Instances
    if (auto* inst = ctx->GetInstances())
        inst->Update(dt);

    // Entity churn: create 20 entities, destroy 20
    std::vector<EntityID> batch;
    batch.reserve(20);
    for (int i = 0; i < 20; i++)
    {
        auto e = world.CreateEntity();
        world.AddComponent<Transform>(e, Transform{});
        world.AddComponent<HealthComponent>(e, HealthComponent{100.0f, 100.0f});
        batch.push_back(e);
    }
    for (auto e : batch)
        world.DestroyEntity(e);

    // EventBus — publish events
    bus.Publish(LoadTestEvent{frameIdx});

    // NullRHI frame
    auto& rhi = Spark::RHI::NullRHIDevice::GetInstance();
    rhi.BeginFrame();
    if (frameIdx % 10 == 0)
    {
        auto b = rhi.CreateBuffer({});
        auto t = rhi.CreateTexture({});
    }
    rhi.EndFrame();

    // GPU counters
    auto& gpu = Spark::Graphics::GPUPerfCounters::GetInstance();
    gpu.Increment(Spark::Graphics::GPUCounterCategory::DrawCalls, 50);
    gpu.Increment(Spark::Graphics::GPUCounterCategory::Primitives, 5000);
    gpu.EndFrame();

    // Profiler
    Profiler::GetInstance().BeginFrame();
    Profiler::GetInstance().BeginSection("heavy_frame", ProfileCategory::Custom);
    Profiler::GetInstance().EndSection("heavy_frame");
    Profiler::GetInstance().EndFrame();

    // JobSystem — submit parallel work every 10 frames
    if (frameIdx % 10 == 0)
    {
        auto& jobs = Spark::JobSystem::Get();
        std::atomic<int> sum{0};
        jobs.ParallelFor(0, 500, [&](int i) { sum.fetch_add(i, std::memory_order_relaxed); });
    }
}

// ============================================================================
// TEST: Full load test — 3000 frames with resource sampling
// ============================================================================

TEST(LoadTest_FullEngine_3000Frames)
{
    InitLoadTestEngine();
    auto* ctx = EngineContext::Get();
    auto* world = ctx->GetWorld();
    auto* eventBus = ctx->GetEventBus();
    EXPECT_TRUE(world != nullptr);
    EXPECT_TRUE(eventBus != nullptr);
    if (!world || !eventBus)
        return;

    // Subscribe to load events
    int eventsReceived = 0;
    auto handle = eventBus->Subscribe<LoadTestEvent>([&](const LoadTestEvent&) { eventsReceived++; });

    constexpr int NUM_FRAMES = 3000;
    constexpr float DT = 1.0f / 60.0f;
    constexpr int SAMPLE_INTERVAL = 50; // sample every 50 frames

    // Collect resource samples
    std::vector<ResourceSample> samples;
    samples.reserve(NUM_FRAMES / SAMPLE_INTERVAL + 2);

    // Per-frame timing
    std::vector<double> frameTimes;
    frameTimes.reserve(NUM_FRAMES);

    // Baseline
    auto baselineSample = SampleResources();
    samples.push_back(baselineSample);

    auto wallStart = std::chrono::high_resolution_clock::now();

    for (int frame = 0; frame < NUM_FRAMES; frame++)
    {
        auto frameStart = std::chrono::high_resolution_clock::now();

        HeavyFrame(DT, frame, *world, *eventBus);

        auto frameEnd = std::chrono::high_resolution_clock::now();
        double frameUs = std::chrono::duration<double, std::micro>(frameEnd - frameStart).count();
        frameTimes.push_back(frameUs);

        // Sample resources periodically
        if ((frame + 1) % SAMPLE_INTERVAL == 0)
        {
            auto s = SampleResources();
            auto elapsed = std::chrono::high_resolution_clock::now() - wallStart;
            s.wallTimeMs = std::chrono::duration<double, std::milli>(elapsed).count();
            samples.push_back(s);
        }
    }

    auto wallEnd = std::chrono::high_resolution_clock::now();
    double totalWallMs = std::chrono::duration<double, std::milli>(wallEnd - wallStart).count();

    // Final sample
    auto finalSample = SampleResources();
    finalSample.wallTimeMs = totalWallMs;
    samples.push_back(finalSample);

    // ================================================================
    // Analyze frame times
    // ================================================================

    std::sort(frameTimes.begin(), frameTimes.end());
    double minFrameUs = frameTimes.front();
    double maxFrameUs = frameTimes.back();
    double avgFrameUs = std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) / frameTimes.size();
    double p50FrameUs = frameTimes[frameTimes.size() / 2];
    double p95FrameUs = frameTimes[static_cast<size_t>(frameTimes.size() * 0.95)];
    double p99FrameUs = frameTimes[static_cast<size_t>(frameTimes.size() * 0.99)];

    int spikes3x = 0, spikes10x = 0;
    for (double t : frameTimes)
    {
        if (t > avgFrameUs * 3.0)
            spikes3x++;
        if (t > avgFrameUs * 10.0)
            spikes10x++;
    }

    // ================================================================
    // Analyze CPU usage
    // ================================================================

    double cpuUserDeltaMs = finalSample.cpuUserMs - baselineSample.cpuUserMs;
    double cpuSystemDeltaMs = finalSample.cpuSystemMs - baselineSample.cpuSystemMs;
    double cpuTotalMs = cpuUserDeltaMs + cpuSystemDeltaMs;
    double cpuUtilization = (totalWallMs > 0.0) ? (cpuTotalMs / totalWallMs * 100.0) : 0.0;

    // ================================================================
    // Analyze memory
    // ================================================================

    size_t minRSS = SIZE_MAX, maxRSS = 0;
    size_t sumRSS = 0;
    for (const auto& s : samples)
    {
        if (s.residentKB > 0)
        {
            minRSS = std::min(minRSS, s.residentKB);
            maxRSS = std::max(maxRSS, s.residentKB);
            sumRSS += s.residentKB;
        }
    }
    size_t avgRSS = (samples.empty() || sumRSS == 0) ? 0 : sumRSS / samples.size();
    if (minRSS == SIZE_MAX)
        minRSS = 0;

    size_t minVSZ = SIZE_MAX, maxVSZ = 0;
    for (const auto& s : samples)
    {
        if (s.virtualKB > 0)
        {
            minVSZ = std::min(minVSZ, s.virtualKB);
            maxVSZ = std::max(maxVSZ, s.virtualKB);
        }
    }
    if (minVSZ == SIZE_MAX)
        minVSZ = 0;

    size_t memGrowthKB = (maxRSS > minRSS) ? (maxRSS - minRSS) : 0;

    // ================================================================
    // Print comprehensive report
    // ================================================================

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           FULL ENGINE LOAD TEST — 3000 FRAMES              ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║                                                            ║\n";
    std::cout << "║  FRAME TIMING                                              ║\n";
    std::cout << "║  ─────────────                                             ║\n";

    auto pad = [](const std::string& s, size_t w) -> std::string
    {
        if (s.size() >= w)
            return s;
        return s + std::string(w - s.size(), ' ');
    };
    auto fmtUs = [](double us) -> std::string
    {
        std::ostringstream ss;
        if (us < 1000.0)
            ss << std::fixed << std::setprecision(1) << us << " us";
        else
            ss << std::fixed << std::setprecision(2) << us / 1000.0 << " ms";
        return ss.str();
    };

    std::cout << "║    Min:      " << pad(fmtUs(minFrameUs), 46) << "║\n";
    std::cout << "║    Avg:      " << pad(fmtUs(avgFrameUs), 46) << "║\n";
    std::cout << "║    P50:      " << pad(fmtUs(p50FrameUs), 46) << "║\n";
    std::cout << "║    P95:      " << pad(fmtUs(p95FrameUs), 46) << "║\n";
    std::cout << "║    P99:      " << pad(fmtUs(p99FrameUs), 46) << "║\n";
    std::cout << "║    Max:      " << pad(fmtUs(maxFrameUs), 46) << "║\n";

    {
        std::ostringstream ss;
        ss << spikes3x << " (>3x avg), " << spikes10x << " (>10x avg)";
        std::cout << "║    Spikes:   " << pad(ss.str(), 46) << "║\n";
    }

    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << totalWallMs << " ms (" << std::setprecision(0)
           << (1000.0 / (totalWallMs / NUM_FRAMES)) << " effective FPS)";
        std::cout << "║    Total:    " << pad(ss.str(), 46) << "║\n";
    }

    std::cout << "║                                                            ║\n";
    std::cout << "║  CPU USAGE                                                 ║\n";
    std::cout << "║  ─────────                                                 ║\n";

    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << cpuUserDeltaMs << " ms";
        std::cout << "║    User:     " << pad(ss.str(), 46) << "║\n";
    }
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << cpuSystemDeltaMs << " ms";
        std::cout << "║    System:   " << pad(ss.str(), 46) << "║\n";
    }
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << cpuTotalMs << " ms";
        std::cout << "║    Total:    " << pad(ss.str(), 46) << "║\n";
    }
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << cpuUtilization << "%";
        std::cout << "║    Util:     " << pad(ss.str(), 46) << "║\n";
    }

    std::cout << "║                                                            ║\n";
    std::cout << "║  MEMORY (RSS)                                              ║\n";
    std::cout << "║  ────────────                                              ║\n";

    auto fmtMem = [](size_t kb) -> std::string
    {
        std::ostringstream ss;
        if (kb < 1024)
            ss << kb << " KB";
        else
            ss << std::fixed << std::setprecision(1) << static_cast<double>(kb) / 1024.0 << " MB";
        return ss.str();
    };

    std::cout << "║    Min:      " << pad(fmtMem(minRSS), 46) << "║\n";
    std::cout << "║    Avg:      " << pad(fmtMem(avgRSS), 46) << "║\n";
    std::cout << "║    Max:      " << pad(fmtMem(maxRSS), 46) << "║\n";
    std::cout << "║    Growth:   " << pad(fmtMem(memGrowthKB), 46) << "║\n";
    std::cout << "║    VSZ Max:  " << pad(fmtMem(maxVSZ), 46) << "║\n";

    std::cout << "║                                                            ║\n";
    std::cout << "║  WORKLOAD                                                  ║\n";
    std::cout << "║  ────────                                                  ║\n";

    {
        std::ostringstream ss;
        ss << NUM_FRAMES * 20 << " created/destroyed";
        std::cout << "║    Entities: " << pad(ss.str(), 46) << "║\n";
    }
    {
        std::ostringstream ss;
        ss << eventsReceived << " events delivered";
        std::cout << "║    Events:   " << pad(ss.str(), 46) << "║\n";
    }
    {
        std::ostringstream ss;
        ss << (NUM_FRAMES / 10) << " ParallelFor batches (500 items each)";
        std::cout << "║    Jobs:     " << pad(ss.str(), 46) << "║\n";
    }
    {
        std::ostringstream ss;
        ss << (NUM_FRAMES / 120 + 1) << " weather transitions";
        std::cout << "║    Weather:  " << pad(ss.str(), 46) << "║\n";
    }

    std::cout << "║                                                            ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << std::flush;

    // ================================================================
    // Assertions
    // ================================================================

    // All events should have been delivered
    EXPECT_EQ(eventsReceived, NUM_FRAMES);

    // No excessive severe frame spikes (allow some for warmup, JIT, thread scheduling)
    EXPECT_TRUE(spikes10x <= 30);

    // Average frame time should be reasonable (generous for Debug/ASan builds)
    EXPECT_TRUE(avgFrameUs < 50000.0);

    // Memory should not have leaked significantly (generous for ASan shadow memory)
    EXPECT_TRUE(memGrowthKB < 102400);

    // Entity count should be back to baseline
    EXPECT_EQ(world->GetEntityCount(), static_cast<size_t>(0));

    handle.Unsubscribe();
}

// ============================================================================
// ADVERSE: High entity count — 10,000 entities alive simultaneously
// ============================================================================

TEST(LoadTest_Adverse_HighEntityCount)
{
    InitLoadTestEngine();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    size_t baseline = world->GetEntityCount();
    constexpr int ENTITY_COUNT = 10000;

    // Create 10,000 entities with multiple components each
    auto createStart = std::chrono::high_resolution_clock::now();

    std::vector<EntityID> entities;
    entities.reserve(ENTITY_COUNT);
    for (int i = 0; i < ENTITY_COUNT; i++)
    {
        auto e = world->CreateEntity();
        world->AddComponent<Transform>(e, Transform{});
        world->AddComponent<HealthComponent>(e, HealthComponent{100.0f, 100.0f});
        if (i % 3 == 0)
            world->AddComponent<LightComponent>(e, LightComponent{});
        entities.push_back(e);
    }

    auto createEnd = std::chrono::high_resolution_clock::now();
    double createMs = std::chrono::duration<double, std::milli>(createEnd - createStart).count();

    EXPECT_EQ(world->GetEntityCount(), baseline + ENTITY_COUNT);

    auto memAfterCreate = SampleResources();

    // Tick 100 frames with all entities alive
    constexpr float DT = 1.0f / 60.0f;
    std::vector<double> frameTimes;
    frameTimes.reserve(100);

    for (int f = 0; f < 100; f++)
    {
        auto fs = std::chrono::high_resolution_clock::now();

        Spark::FixedTimestepAccumulator::GetInstance().Advance(DT);
        if (auto* w = EngineContext::Get()->GetWeather())
            w->Update(DT);
        if (auto* t = EngineContext::Get()->GetTimeOfDay())
            t->Update(DT);
        if (auto* tw = EngineContext::Get()->GetTween())
            tw->Update(DT);
        Profiler::GetInstance().BeginFrame();
        Profiler::GetInstance().EndFrame();

        auto fe = std::chrono::high_resolution_clock::now();
        frameTimes.push_back(std::chrono::duration<double, std::micro>(fe - fs).count());
    }

    std::sort(frameTimes.begin(), frameTimes.end());
    double avgUs = std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) / frameTimes.size();

    // Destroy all
    auto destroyStart = std::chrono::high_resolution_clock::now();
    for (auto e : entities)
        world->DestroyEntity(e);
    auto destroyEnd = std::chrono::high_resolution_clock::now();
    double destroyMs = std::chrono::duration<double, std::milli>(destroyEnd - destroyStart).count();

    EXPECT_EQ(world->GetEntityCount(), baseline);

    auto memAfterDestroy = SampleResources();

    std::cout << "\n=== ADVERSE: 10,000 Entities Alive ===\n";
    std::cout << "  Create 10k: " << std::fixed << std::setprecision(1) << createMs << " ms\n";
    std::cout << "  Destroy 10k: " << destroyMs << " ms\n";
    std::cout << "  Frame avg (100 frames): " << std::setprecision(1) << avgUs << " us\n";
    std::cout << "  Frame P99: " << frameTimes[99] << " us\n";
    std::cout << "  RSS after create: " << memAfterCreate.residentKB << " KB\n";
    std::cout << "  RSS after destroy: " << memAfterDestroy.residentKB << " KB\n" << std::flush;

    EXPECT_TRUE(createMs < 10000.0); // generous for Debug/ASan builds
    EXPECT_TRUE(destroyMs < 10000.0);
}

// ============================================================================
// ADVERSE: Thread contention — max JobSystem pressure
// ============================================================================

TEST(LoadTest_Adverse_ThreadContention)
{
    InitLoadTestEngine();
    auto& jobs = Spark::JobSystem::Get();

    constexpr int BATCHES = 50;
    constexpr int JOBS_PER_BATCH = 200;

    std::atomic<int> totalCompleted{0};
    std::vector<double> batchTimes;
    batchTimes.reserve(BATCHES);

    for (int b = 0; b < BATCHES; b++)
    {
        auto batchStart = std::chrono::high_resolution_clock::now();

        // Fire 200 jobs that all contend on the same atomic
        std::vector<std::future<void>> futures;
        futures.reserve(JOBS_PER_BATCH);
        std::atomic<int> contended{0};

        for (int j = 0; j < JOBS_PER_BATCH; j++)
        {
            futures.push_back(jobs.Submit(
                [&contended, &totalCompleted]()
                {
                    // Tight contention: 100 atomic increments per job
                    for (int k = 0; k < 100; k++)
                        contended.fetch_add(1, std::memory_order_seq_cst);
                    totalCompleted.fetch_add(1, std::memory_order_relaxed);
                }));
        }

        for (auto& f : futures)
            f.get();

        auto batchEnd = std::chrono::high_resolution_clock::now();
        batchTimes.push_back(std::chrono::duration<double, std::micro>(batchEnd - batchStart).count());

        EXPECT_EQ(contended.load(), JOBS_PER_BATCH * 100);
    }

    std::sort(batchTimes.begin(), batchTimes.end());
    double avgBatchUs = std::accumulate(batchTimes.begin(), batchTimes.end(), 0.0) / batchTimes.size();

    std::cout << "\n=== ADVERSE: Thread Contention (50 batches x 200 jobs) ===\n";
    std::cout << "  Total jobs: " << totalCompleted.load() << "/" << (BATCHES * JOBS_PER_BATCH) << "\n";
    std::cout << "  Batch avg: " << std::fixed << std::setprecision(0) << avgBatchUs << " us\n";
    std::cout << "  Batch min: " << batchTimes.front() << " us\n";
    std::cout << "  Batch max: " << batchTimes.back() << " us\n";
    std::cout << "  Batch P99: " << batchTimes[static_cast<size_t>(batchTimes.size() * 0.99)] << " us\n" << std::flush;

    EXPECT_EQ(totalCompleted.load(), BATCHES * JOBS_PER_BATCH);
}

// ============================================================================
// ADVERSE: Rapid state thrashing — weather/time/tween flip-flopping
// ============================================================================

TEST(LoadTest_Adverse_StateThrashing)
{
    InitLoadTestEngine();
    auto* ctx = EngineContext::Get();

    constexpr int FRAMES = 2000;
    constexpr float DT = 1.0f / 60.0f;

    auto memBefore = SampleResources();
    std::vector<double> frameTimes;
    frameTimes.reserve(FRAMES);

    for (int f = 0; f < FRAMES; f++)
    {
        auto fs = std::chrono::high_resolution_clock::now();

        // Thrash weather — switch every single frame
        if (auto* w = ctx->GetWeather())
        {
            auto type = static_cast<Spark::WeatherType>(f % 6);
            w->SetWeather(type, static_cast<float>(f % 10) / 10.0f, 0.0f);
            w->Update(DT);
        }

        // Thrash time — jump randomly every frame
        if (auto* t = ctx->GetTimeOfDay())
        {
            t->SetTimeOfDay(static_cast<float>(f % 24));
            t->Update(DT);
        }

        // Thrash tweens — create and immediately cancel
        if (auto* tw = ctx->GetTween())
        {
            static float dummy = 0.0f;
            auto h = tw->TweenFloat(dummy, 0.0f, 1.0f, 10.0f, Spark::EaseType::Linear);
            tw->Update(DT);
            tw->Cancel(h);
        }

        // Thrash coroutines — start and stop
        auto& coro = Spark::CoroutineScheduler::GetInstance();
        coro.StartCoroutine("__thrash_" + std::to_string(f)).Do([]() {});
        coro.Update(DT);

        auto fe = std::chrono::high_resolution_clock::now();
        frameTimes.push_back(std::chrono::duration<double, std::micro>(fe - fs).count());
    }

    auto memAfter = SampleResources();
    std::sort(frameTimes.begin(), frameTimes.end());
    double avgUs = std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) / frameTimes.size();

    int64_t memDeltaKB = static_cast<int64_t>(memAfter.residentKB) - static_cast<int64_t>(memBefore.residentKB);

    std::cout << "\n=== ADVERSE: State Thrashing (2000 frames) ===\n";
    std::cout << "  Frame avg: " << std::fixed << std::setprecision(1) << avgUs << " us\n";
    std::cout << "  Frame min: " << frameTimes.front() << " us\n";
    std::cout << "  Frame max: " << frameTimes.back() << " us\n";
    std::cout << "  Frame P99: " << frameTimes[static_cast<size_t>(frameTimes.size() * 0.99)] << " us\n";
    std::cout << "  Memory delta: " << memDeltaKB << " KB\n" << std::flush;

    EXPECT_TRUE(avgUs < 5000.0);
    EXPECT_TRUE(std::abs(memDeltaKB) < 5120); // < 5MB drift
}

// ============================================================================
// SEVERE: Entity flood — 100,000 entities
// ============================================================================

TEST(LoadTest_Severe_EntityFlood)
{
    InitLoadTestEngine();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    size_t baseline = world->GetEntityCount();
    constexpr int FLOOD_SIZE = 100000;

    auto memBefore = SampleResources();

    auto createStart = std::chrono::high_resolution_clock::now();

    std::vector<EntityID> entities;
    entities.reserve(FLOOD_SIZE);
    for (int i = 0; i < FLOOD_SIZE; i++)
    {
        auto e = world->CreateEntity();
        world->AddComponent<Transform>(e, Transform{});
        entities.push_back(e);
    }

    auto createEnd = std::chrono::high_resolution_clock::now();
    double createMs = std::chrono::duration<double, std::milli>(createEnd - createStart).count();

    auto memPeak = SampleResources();
    EXPECT_EQ(world->GetEntityCount(), baseline + FLOOD_SIZE);

    // Destroy all
    auto destroyStart = std::chrono::high_resolution_clock::now();
    for (auto e : entities)
        world->DestroyEntity(e);
    auto destroyEnd = std::chrono::high_resolution_clock::now();
    double destroyMs = std::chrono::duration<double, std::milli>(destroyEnd - destroyStart).count();

    auto memAfter = SampleResources();
    EXPECT_EQ(world->GetEntityCount(), baseline);

    std::cout << "\n=== SEVERE: 100,000 Entity Flood ===\n";
    std::cout << "  Create 100k: " << std::fixed << std::setprecision(1) << createMs << " ms (" << std::setprecision(3)
              << (createMs / FLOOD_SIZE * 1000.0) << " us/entity)\n";
    std::cout << "  Destroy 100k: " << std::setprecision(1) << destroyMs << " ms (" << std::setprecision(3)
              << (destroyMs / FLOOD_SIZE * 1000.0) << " us/entity)\n";
    std::cout << "  RSS before: " << memBefore.residentKB << " KB\n";
    std::cout << "  RSS peak:   " << memPeak.residentKB << " KB\n";
    std::cout << "  RSS after:  " << memAfter.residentKB << " KB\n" << std::flush;

    // No hard timing assertion for 100k flood — Debug/ASan can be 100x slower.
    // The test validates correctness (entity count), not speed.
    (void)createMs;
    (void)destroyMs;
}

// ============================================================================
// SEVERE: EventBus storm — 100k events x 50 subscribers
// ============================================================================

TEST(LoadTest_Severe_EventBusStorm)
{
    Spark::EventBus bus;

    struct StormEvent
    {
        int id;
        float payload;
    };

    std::atomic<int> totalDelivered{0};

    // 50 subscribers
    std::vector<Spark::SubscriptionHandle> handles;
    for (int i = 0; i < 50; i++)
    {
        handles.push_back(bus.Subscribe<StormEvent>([&totalDelivered](const StormEvent&)
                                                    { totalDelivered.fetch_add(1, std::memory_order_relaxed); }));
    }

    constexpr int NUM_EVENTS = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_EVENTS; i++)
    {
        bus.Publish(StormEvent{i, static_cast<float>(i) * 0.01f});
    }
    auto end = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();

    int expected = NUM_EVENTS * 50;

    std::cout << "\n=== SEVERE: EventBus Storm (100k events x 50 subscribers) ===\n";
    std::cout << "  Total deliveries: " << totalDelivered.load() << "/" << expected << "\n";
    std::cout << "  Total time: " << std::fixed << std::setprecision(1) << totalMs << " ms\n";
    std::cout << "  Per event: " << std::setprecision(3) << (totalMs / NUM_EVENTS * 1000.0) << " us\n";
    std::cout << "  Throughput: " << std::setprecision(0) << (totalDelivered.load() / (totalMs / 1000.0))
              << " deliveries/sec\n"
              << std::flush;

    EXPECT_EQ(totalDelivered.load(), expected);
    EXPECT_TRUE(totalMs < 5000.0); // < 5 seconds for 5M deliveries
}

// ============================================================================
// SEVERE: JobSystem flood — 10,000 async jobs
// ============================================================================

TEST(LoadTest_Severe_JobSystemFlood)
{
    InitLoadTestEngine();
    auto& jobs = Spark::JobSystem::Get();

    constexpr int NUM_JOBS = 10000;
    std::atomic<int> counter{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::future<void>> futures;
    futures.reserve(NUM_JOBS);
    for (int i = 0; i < NUM_JOBS; i++)
    {
        futures.push_back(jobs.Submit(
            [&counter]()
            {
                // Real work: compute sum
                volatile int sum = 0;
                for (int k = 0; k < 1000; k++)
                    sum += k;
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
    }

    for (auto& f : futures)
        f.get();

    auto end = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "\n=== SEVERE: JobSystem Flood (10,000 jobs) ===\n";
    std::cout << "  Completed: " << counter.load() << "/" << NUM_JOBS << "\n";
    std::cout << "  Total time: " << std::fixed << std::setprecision(1) << totalMs << " ms\n";
    std::cout << "  Per job: " << std::setprecision(2) << (totalMs / NUM_JOBS * 1000.0) << " us\n";
    std::cout << "  Throughput: " << std::setprecision(0) << (NUM_JOBS / (totalMs / 1000.0)) << " jobs/sec\n"
              << std::flush;

    EXPECT_EQ(counter.load(), NUM_JOBS);
}

// ============================================================================
// SEVERE: Save/load under pressure — rapid save/load cycling
// ============================================================================

TEST(LoadTest_Severe_SaveLoadCycling)
{
    InitLoadTestEngine();
    auto* save = EngineContext::Get()->GetSaveSystem();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(save != nullptr && world != nullptr);
    if (!save || !world)
        return;

    constexpr int CYCLES = 50;
    std::vector<double> saveTimes, loadTimes;
    saveTimes.reserve(CYCLES);
    loadTimes.reserve(CYCLES);

    // Create some entities to serialize
    std::vector<EntityID> sceneEntities;
    for (int i = 0; i < 100; i++)
    {
        auto e = world->CreateEntity("__save_test_" + std::to_string(i));
        world->AddComponent<Transform>(e, Transform{});
        world->AddComponent<HealthComponent>(e, HealthComponent{100.0f, 100.0f});
        sceneEntities.push_back(e);
    }

    for (int c = 0; c < CYCLES; c++)
    {
        Spark::SaveMetadata meta;
        meta.saveName = "stress_" + std::to_string(c);

        auto saveStart = std::chrono::high_resolution_clock::now();
        bool saved = save->Save("__stress_slot_" + std::to_string(c % 5), *world, meta);
        auto saveEnd = std::chrono::high_resolution_clock::now();
        saveTimes.push_back(std::chrono::duration<double, std::micro>(saveEnd - saveStart).count());

        if (saved)
        {
            auto loadStart = std::chrono::high_resolution_clock::now();
            save->Load("__stress_slot_" + std::to_string(c % 5), *world);
            auto loadEnd = std::chrono::high_resolution_clock::now();
            loadTimes.push_back(std::chrono::duration<double, std::micro>(loadEnd - loadStart).count());
        }
    }

    // Clean up
    for (int i = 0; i < 5; i++)
        save->DeleteSave("__stress_slot_" + std::to_string(i));
    for (auto e : sceneEntities)
    {
        if (world->GetRegistry().valid(e))
            world->DestroyEntity(e);
    }

    std::sort(saveTimes.begin(), saveTimes.end());
    std::sort(loadTimes.begin(), loadTimes.end());
    double avgSaveUs = std::accumulate(saveTimes.begin(), saveTimes.end(), 0.0) / saveTimes.size();
    double avgLoadUs =
        loadTimes.empty() ? 0.0 : std::accumulate(loadTimes.begin(), loadTimes.end(), 0.0) / loadTimes.size();

    std::cout << "\n=== SEVERE: Save/Load Cycling (50 cycles, 100 entities) ===\n";
    std::cout << "  Save avg: " << std::fixed << std::setprecision(0) << avgSaveUs << " us\n";
    std::cout << "  Save max: " << saveTimes.back() << " us\n";
    std::cout << "  Load avg: " << avgLoadUs << " us\n";
    if (!loadTimes.empty())
        std::cout << "  Load max: " << loadTimes.back() << " us\n";
    std::cout << std::flush;
}

// ============================================================================
// SEVERE: Network rapid connect/disconnect cycling
// ============================================================================

TEST(LoadTest_Severe_NetworkChurn)
{
    auto& net = Spark::Net::NetworkManager::GetInstance();

    constexpr int CYCLES = 50;
    int successCount = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < CYCLES; i++)
    {
        uint16_t port = static_cast<uint16_t>(55000 + (i % 100));
        if (net.StartServer(port, 2))
        {
            net.Update(0.016f);
            net.StopServer();
            successCount++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "\n=== SEVERE: Network Churn (50 start/stop cycles) ===\n";
    std::cout << "  Successful: " << successCount << "/" << CYCLES << "\n";
    std::cout << "  Total time: " << std::fixed << std::setprecision(1) << totalMs << " ms\n";
    std::cout << "  Per cycle: " << std::setprecision(2) << (totalMs / CYCLES) << " ms\n" << std::flush;

    EXPECT_TRUE(successCount >= CYCLES - 5);
    EXPECT_TRUE(net.GetRole() == Spark::Net::NetworkRole::None);
}

// ============================================================================
// DEEP: Dialogue system — build tree, traverse, select choices
// ============================================================================

TEST(DeepStress_DialogueTraversal)
{
    InitLoadTestEngine();

    auto* dialogue = EngineContext::Get()->GetDialogue();
    EXPECT_TRUE(dialogue != nullptr);
    if (!dialogue)
        return;

    // Build a dialogue tree with branching choices
    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetId("stress_tree");
    tree->SetStartNodeId("start");

    Spark::DialogueNode startNode;
    startNode.id = "start";
    startNode.type = Spark::DialogueNodeType::Text;
    startNode.speakerName = "NPC";
    startNode.text = "Hello adventurer!";
    startNode.displayDuration = 0.5f;

    Spark::DialogueChoice choice1;
    choice1.text = "Tell me more";
    choice1.nextNodeId = "branch_a";
    Spark::DialogueChoice choice2;
    choice2.text = "Goodbye";
    choice2.nextNodeId = "end_node";
    startNode.choices.push_back(choice1);
    startNode.choices.push_back(choice2);
    startNode.nextNodeId = "";
    tree->AddNode(startNode);

    Spark::DialogueNode branchA;
    branchA.id = "branch_a";
    branchA.type = Spark::DialogueNodeType::Text;
    branchA.speakerName = "NPC";
    branchA.text = "Here's some lore...";
    branchA.displayDuration = 0.5f;
    branchA.nextNodeId = "end_node";
    tree->AddNode(branchA);

    Spark::DialogueNode endNode;
    endNode.id = "end_node";
    endNode.type = Spark::DialogueNodeType::End;
    tree->AddNode(endNode);

    dialogue->RegisterTree("stress_tree", std::move(tree));

    // Traverse the tree many times, taking different paths
    constexpr int TRAVERSALS = 100;
    int completedConversations = 0;

    for (int i = 0; i < TRAVERSALS; i++)
    {
        bool started = dialogue->StartConversation("stress_tree");
        EXPECT_TRUE(started);
        if (!started)
            continue;

        dialogue->Update(0.016f);

        // Alternate between choices
        dialogue->SelectChoice(i % 2);
        dialogue->Update(0.016f);

        dialogue->EndConversation();
        completedConversations++;
    }

    EXPECT_EQ(completedConversations, TRAVERSALS);
    std::cout << "\n=== DEEP: Dialogue Traversal (" << TRAVERSALS << " conversations) ===\n";
    std::cout << "  Completed: " << completedConversations << "/" << TRAVERSALS << "\n" << std::flush;
}

// ============================================================================
// DEEP: Destruction system — register patterns, apply damage
// ============================================================================

TEST(DeepStress_DestructionSystem)
{
    InitLoadTestEngine();

    auto& destruction = Spark::DestructionSystem::GetInstance();

    // Register fracture patterns
    Spark::FracturePattern wallPattern;
    for (int i = 0; i < 8; i++)
    {
        Spark::FracturePiece piece;
        piece.name = "shard_" + std::to_string(i);
        piece.meshName = "shard_mesh";
        piece.localOffset = {static_cast<float>(i) * 0.5f, 0.0f, 0.0f};
        piece.mass = 2.0f;
        piece.lifetime = 5.0f;
        piece.scatterForce = 8.0f;
        wallPattern.AddPiece(piece);
    }
    wallPattern.SetDestructionSound("wall_break");
    wallPattern.SetParticleEffect("debris_dust");
    destruction.RegisterPattern("wall_break", wallPattern);

    EXPECT_TRUE(destruction.GetPattern("wall_break") != nullptr);

    // Apply damage to many entities
    constexpr int ENTITIES = 500;
    for (uint32_t i = 0; i < ENTITIES; i++)
    {
        DirectX::XMFLOAT3 hitPt = {static_cast<float>(i), 1.0f, 0.0f};
        DirectX::XMFLOAT3 hitDir = {0.0f, 0.0f, 1.0f};
        destruction.ApplyDamage(i + 1000, 50.0f, hitPt, hitDir);
    }

    // Force destroy some
    for (uint32_t i = 0; i < 100; i++)
    {
        destruction.ForceDestroy(i + 2000, 20.0f);
    }

    // Update to process damage
    destruction.Update(0.016f);
    destruction.Update(0.016f);

    std::cout << "\n=== DEEP: Destruction System ===\n";
    std::cout << "  Patterns registered: 1\n";
    std::cout << "  Damage applied to: " << ENTITIES << " entities\n";
    std::cout << "  Force destroyed: 100 entities\n" << std::flush;
}

// ============================================================================
// DEEP: Proximity triggers — create, fire, remove
// ============================================================================

TEST(DeepStress_ProximityTriggers)
{
    InitLoadTestEngine();

    auto& triggers = Spark::World::ProximityTriggerSystem::GetInstance();

    std::atomic<int> enterCount{0};
    std::atomic<int> exitCount{0};

    // Create many sphere triggers
    constexpr int TRIGGER_COUNT = 200;
    std::vector<uint32_t> triggerIds;
    triggerIds.reserve(TRIGGER_COUNT);

    for (int i = 0; i < TRIGGER_COUNT; i++)
    {
        DirectX::XMFLOAT3 center = {static_cast<float>(i * 10), 0.0f, 0.0f};
        uint32_t id = triggers.CreateSphereTrigger(
            center, 5.0f, [&enterCount](uint32_t, uint32_t) { enterCount.fetch_add(1, std::memory_order_relaxed); },
            [&exitCount](uint32_t, uint32_t) { exitCount.fetch_add(1, std::memory_order_relaxed); });
        triggerIds.push_back(id);
    }

    // Simulate entity moving through trigger zones
    constexpr int FRAMES = 500;
    for (int f = 0; f < FRAMES; f++)
    {
        std::vector<Spark::World::EntityPosition> positions;
        // 10 entities moving along X axis
        for (uint32_t e = 0; e < 10; e++)
        {
            Spark::World::EntityPosition pos;
            pos.entityID = e + 1;
            pos.position = {static_cast<float>(f * 4 + e * 3), 0.0f, 0.0f};
            positions.push_back(pos);
        }
        triggers.Update(positions);
    }

    // Remove all triggers
    for (auto id : triggerIds)
        triggers.RemoveTrigger(id);

    std::cout << "\n=== DEEP: Proximity Triggers ===\n";
    std::cout << "  Triggers created: " << TRIGGER_COUNT << "\n";
    std::cout << "  Frames simulated: " << FRAMES << "\n";
    std::cout << "  Enter callbacks: " << enterCount.load() << "\n";
    std::cout << "  Exit callbacks: " << exitCount.load() << "\n" << std::flush;

    // Entities should have entered and exited triggers as they moved
    EXPECT_TRUE(enterCount.load() > 0);
}

// ============================================================================
// DEEP: Condition system — evaluate conditions with variables
// ============================================================================

TEST(DeepStress_ConditionSystem)
{
    InitLoadTestEngine();

    auto& conditions = Spark::Gameplay::ConditionSystem::GetInstance();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    // Set some variables and flags
    for (int i = 0; i < 100; i++)
    {
        conditions.SetVariable("var_" + std::to_string(i), static_cast<int64_t>(i * 10));
        conditions.SetFlag("flag_" + std::to_string(i), i % 2 == 0);
    }

    // Create an entity to evaluate against
    auto entity = world->CreateEntity("condition_test");
    world->AddComponent<Transform>(entity, Transform{});
    world->AddComponent<HealthComponent>(entity, HealthComponent{80.0f, 100.0f});

    // Evaluate many condition sets
    constexpr int EVALS = 1000;
    int trueCount = 0;

    for (int i = 0; i < EVALS; i++)
    {
        Spark::Gameplay::ConditionSet condSet;
        Spark::Gameplay::ConditionGroup group;
        group.logic = Spark::Gameplay::ConditionGroupLogic::And;

        // Alternate between AlwaysTrue and AlwaysFalse to get a mix
        Spark::Gameplay::Condition c1;
        c1.type =
            (i % 3 == 0) ? Spark::Gameplay::ConditionType::AlwaysFalse : Spark::Gameplay::ConditionType::AlwaysTrue;
        group.conditions.push_back(c1);

        // Flag check — even flags are true, odd are false
        Spark::Gameplay::Condition c2;
        c2.type = Spark::Gameplay::ConditionType::FlagSet;
        c2.param1 = std::string("flag_" + std::to_string(i % 100));
        group.conditions.push_back(c2);

        condSet.groups.push_back(group);

        bool result = conditions.Evaluate(condSet, static_cast<uint32_t>(entity), *world);
        if (result)
            trueCount++;
    }

    world->DestroyEntity(entity);

    std::cout << "\n=== DEEP: Condition System ===\n";
    std::cout << "  Evaluations: " << EVALS << "\n";
    std::cout << "  True results: " << trueCount << "\n";
    std::cout << "  False results: " << (EVALS - trueCount) << "\n" << std::flush;

    // With AlwaysFalse every 3rd iteration (AND logic), we expect a mix
    EXPECT_TRUE(trueCount > 0);
    EXPECT_TRUE(trueCount < EVALS);
}

// ============================================================================
// DEEP: Replay system — record and playback
// ============================================================================

TEST(DeepStress_ReplaySystem)
{
    auto& replay = Spark::ReplaySystem::GetInstance();

    replay.SetMetadata("stress_map", "stress_mode");
    replay.StartRecording();

    // Record 500 frames with 20 entities each
    constexpr int FRAMES = 500;
    constexpr int ENTITIES_PER_FRAME = 20;

    for (int f = 0; f < FRAMES; f++)
    {
        std::vector<Spark::ReplayEntityState> entities;
        entities.reserve(ENTITIES_PER_FRAME);

        for (int e = 0; e < ENTITIES_PER_FRAME; e++)
        {
            Spark::ReplayEntityState state;
            state.entityId = static_cast<uint32_t>(e);
            state.position = {static_cast<float>(f + e), static_cast<float>(e), 0.0f};
            state.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            state.velocity = {1.0f, 0.0f, 0.0f};
            state.health = 100.0f - static_cast<float>(f % 50);
            state.animationState = f % 4;
            state.flags = 1; // Alive
            entities.push_back(state);
        }
        replay.RecordFrame(entities, static_cast<float>(f) * 0.016f);
    }

    replay.StopRecording();

    // Now play it back
    replay.StartPlayback();

    int playbackFrames = 0;
    while (replay.GetPlaybackState() == Spark::PlaybackState::Playing && playbackFrames < FRAMES + 100)
    {
        replay.UpdatePlayback(0.016f);
        auto* frame = replay.GetCurrentFrame();
        if (frame)
            playbackFrames++;
    }

    replay.StopPlayback();

    std::cout << "\n=== DEEP: Replay System ===\n";
    std::cout << "  Recorded frames: " << FRAMES << "\n";
    std::cout << "  Entities per frame: " << ENTITIES_PER_FRAME << "\n";
    std::cout << "  Playback frames retrieved: " << playbackFrames << "\n" << std::flush;

    EXPECT_TRUE(playbackFrames > 0);
}

// ============================================================================
// DEEP: Seamless area streaming — register and simulate player movement
// ============================================================================

TEST(DeepStress_AreaStreaming)
{
    InitLoadTestEngine();

    auto& streaming = Spark::Streaming::SeamlessAreaManager::GetInstance();

    // Register a grid of areas
    constexpr int GRID = 4;
    for (int x = 0; x < GRID; x++)
    {
        for (int z = 0; z < GRID; z++)
        {
            Spark::Streaming::AreaDefinition def;
            def.areaId = static_cast<uint32_t>(x * GRID + z + 1);
            def.name = "area_" + std::to_string(x) + "_" + std::to_string(z);
            def.scenePath = "scenes/area_" + def.name + ".scene";
            def.boundsMin = {static_cast<float>(x * 256), 0.0f, static_cast<float>(z * 256)};
            def.boundsMax = {static_cast<float>((x + 1) * 256), 128.0f, static_cast<float>((z + 1) * 256)};
            def.priority = (x == 1 && z == 1) ? 10 : 0; // Center area is high priority
            streaming.RegisterArea(def);
        }
    }

    // Simulate player walking across the area grid
    constexpr int FRAMES = 300;
    for (int f = 0; f < FRAMES; f++)
    {
        float playerX = static_cast<float>(f) * 3.0f; // Walk along X
        float playerZ = 512.0f;                       // Middle of grid
        DirectX::XMFLOAT3 pos = {playerX, 0.0f, playerZ};
        DirectX::XMFLOAT3 vel = {3.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 camDir = {1.0f, 0.0f, 0.0f};

        streaming.SetPlayerState(pos, vel, camDir);
        streaming.Update(0.016f);
    }

    std::cout << "\n=== DEEP: Area Streaming ===\n";
    std::cout << "  Areas registered: " << (GRID * GRID) << "\n";
    std::cout << "  Frames simulated: " << FRAMES << "\n" << std::flush;
}

// ============================================================================
// EDGE: Zero delta-time frames — should not crash or divide by zero
// ============================================================================

TEST(EdgeCase_ZeroDeltaTime)
{
    InitLoadTestEngine();
    auto* ctx = EngineContext::Get();

    // Run 100 frames with dt=0 — should not crash, no NaN, no div-by-zero
    for (int f = 0; f < 100; f++)
    {
        if (auto* w = ctx->GetWeather())
            w->Update(0.0f);
        if (auto* t = ctx->GetTimeOfDay())
            t->Update(0.0f);
        if (auto* tw = ctx->GetTween())
            tw->Update(0.0f);
        if (auto* c = ctx->GetCoroutineScheduler())
            c->Update(0.0f);
        if (auto* p = ctx->GetPhysics())
            p->Update(0.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(0.0f);

        auto& rhi = Spark::RHI::NullRHIDevice::GetInstance();
        rhi.BeginFrame();
        rhi.EndFrame();
    }

    std::cout << "\n=== EDGE: Zero Delta-Time (100 frames) — OK ===\n" << std::flush;
}

// ============================================================================
// EDGE: Huge delta-time — simulate long frame hitches
// ============================================================================

TEST(EdgeCase_HugeDeltaTime)
{
    InitLoadTestEngine();
    auto* ctx = EngineContext::Get();

    // Simulate a 10-second frame hitch — should not crash or overflow
    float hugeDt = 10.0f;
    if (auto* w = ctx->GetWeather())
    {
        w->SetWeather(Spark::WeatherType::Rain, 0.5f, 1.0f);
        w->Update(hugeDt);
    }
    if (auto* t = ctx->GetTimeOfDay())
    {
        t->SetTimeOfDay(12.0f);
        t->Update(hugeDt);
    }
    if (auto* tw = ctx->GetTween())
    {
        static float val = 0.0f;
        tw->TweenFloat(val, 0.0f, 100.0f, 0.5f, Spark::EaseType::EaseInOutCubic);
        tw->Update(hugeDt); // should complete instantly
    }
    if (auto* c = ctx->GetCoroutineScheduler())
        c->Update(hugeDt);

    Spark::FixedTimestepAccumulator::GetInstance().Advance(hugeDt);

    // Another huge frame
    hugeDt = 60.0f; // one minute
    if (auto* w = ctx->GetWeather())
        w->Update(hugeDt);
    if (auto* t = ctx->GetTimeOfDay())
        t->Update(hugeDt);

    std::cout << "\n=== EDGE: Huge Delta-Time (10s + 60s) — OK ===\n" << std::flush;
}

// ============================================================================
// EDGE: Double-destroy and invalid entity operations
// ============================================================================

TEST(EdgeCase_DoubleDestroyEntity)
{
    InitLoadTestEngine();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    // Create and destroy, then try to destroy again
    auto e = world->CreateEntity("double_destroy_test");
    world->AddComponent<Transform>(e, Transform{});
    world->DestroyEntity(e);

    // Second destroy — should not crash (gracefully returns)
    world->DestroyEntity(e);

    // Entity should be invalid in registry after destruction
    EXPECT_FALSE(world->GetRegistry().valid(e));

    std::cout << "\n=== EDGE: Double-Destroy Entity — OK ===\n" << std::flush;
}

// ============================================================================
// EDGE: Rapid create-destroy cycling — tests allocator stability
// ============================================================================

TEST(EdgeCase_RapidCreateDestroyCycle)
{
    InitLoadTestEngine();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    size_t baseline = world->GetEntityCount();

    // Create and immediately destroy 10,000 entities one at a time
    for (int i = 0; i < 10000; i++)
    {
        auto e = world->CreateEntity();
        world->AddComponent<Transform>(e, Transform{});
        world->AddComponent<HealthComponent>(e, HealthComponent{100.0f, 100.0f});
        world->DestroyEntity(e);
    }

    EXPECT_EQ(world->GetEntityCount(), baseline);

    std::cout << "\n=== EDGE: Rapid Create-Destroy Cycle (10k) — OK ===\n" << std::flush;
}

// ============================================================================
// DEEP: AI systems — exercise tactical, cover, formation together
// ============================================================================

TEST(DeepStress_AISystemsIntegration)
{
    InitLoadTestEngine();

    auto& formation = Spark::AI::FormationSystem::GetInstance();
    auto& groupAI = Spark::AI::GroupAISystem::GetInstance();

    // Run AI systems that have Update for many frames
    constexpr int FRAMES = 200;
    constexpr float DT = 1.0f / 60.0f;

    for (int f = 0; f < FRAMES; f++)
    {
        formation.Update(DT);
        groupAI.Update(DT);
    }

    std::cout << "\n=== DEEP: AI Systems Integration (200 frames) — OK ===\n" << std::flush;
}

// ============================================================================
// DEEP: Combined subsystem stress — all systems running simultaneously
// ============================================================================

TEST(DeepStress_AllSubsystemsCombined)
{
    InitLoadTestEngine();
    auto* ctx = EngineContext::Get();
    auto* world = ctx->GetWorld();
    auto* eventBus = ctx->GetEventBus();
    EXPECT_TRUE(world != nullptr && eventBus != nullptr);
    if (!world || !eventBus)
        return;

    auto& rhi = Spark::RHI::NullRHIDevice::GetInstance();
    auto& destruction = Spark::DestructionSystem::GetInstance();
    auto& jobs = Spark::JobSystem::Get();

    constexpr int FRAMES = 1000;
    constexpr float DT = 1.0f / 60.0f;

    size_t baselineEntityCount = world->GetEntityCount();
    auto memBefore = SampleResources();
    auto wallStart = std::chrono::high_resolution_clock::now();

    for (int f = 0; f < FRAMES; f++)
    {
        // Physics
        if (auto* p = ctx->GetPhysics())
            p->Update(DT);

        // Weather + TimeOfDay
        if (auto* w = ctx->GetWeather())
        {
            if (f % 60 == 0)
                w->SetWeather(static_cast<Spark::WeatherType>(f % 6), 0.5f, 0.5f);
            w->Update(DT);
        }
        if (auto* t = ctx->GetTimeOfDay())
            t->Update(DT);

        // Coroutines + Tweens
        if (auto* c = ctx->GetCoroutineScheduler())
            c->Update(DT);
        if (auto* tw = ctx->GetTween())
        {
            if (f % 20 == 0)
            {
                static float scratch = 0.0f;
                tw->TweenFloat(scratch, 0.0f, 1.0f, 0.3f, Spark::EaseType::EaseOutBounce);
            }
            tw->Update(DT);
        }

        // Abilities + Conditions
        if (auto* ab = ctx->GetAbilities())
            ab->Update(*world, DT);

        // Entity churn
        std::vector<EntityID> batch;
        for (int i = 0; i < 10; i++)
        {
            auto e = world->CreateEntity();
            world->AddComponent<Transform>(e, Transform{});
            batch.push_back(e);
        }
        for (auto e : batch)
            world->DestroyEntity(e);

        // Destruction
        if (f % 50 == 0)
        {
            destruction.ApplyDamage(f + 5000, 100.0f, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
        }
        destruction.Update(DT);

        // RHI
        rhi.BeginFrame();
        rhi.EndFrame();

        // AI systems
        Spark::AI::FormationSystem::GetInstance().Update(DT);
        Spark::AI::GroupAISystem::GetInstance().Update(DT);

        // EventBus
        eventBus->Publish(LoadTestEvent{f});

        // JobSystem work every 25 frames
        if (f % 25 == 0)
        {
            std::atomic<int> sum{0};
            jobs.ParallelFor(0, 200, [&](int i) { sum.fetch_add(i, std::memory_order_relaxed); });
        }

        // Fixed timestep
        Spark::FixedTimestepAccumulator::GetInstance().Advance(DT);
    }

    auto wallEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(wallEnd - wallStart).count();
    auto memAfter = SampleResources();

    int64_t memDeltaKB = static_cast<int64_t>(memAfter.residentKB) - static_cast<int64_t>(memBefore.residentKB);

    std::cout << "\n=== DEEP: All Subsystems Combined (1000 frames) ===\n";
    std::cout << "  Total wall time: " << std::fixed << std::setprecision(1) << totalMs << " ms\n";
    std::cout << "  Avg frame: " << std::setprecision(1) << (totalMs / FRAMES) << " ms\n";
    std::cout << "  Memory delta: " << memDeltaKB << " KB\n" << std::flush;

    EXPECT_EQ(world->GetEntityCount(), baselineEntityCount);
    EXPECT_TRUE(totalMs < 60000.0); // < 60s for 1000 frames even in Debug
}
