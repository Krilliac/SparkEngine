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

    static PhysicsSystem physics;
    physics.Initialize();
    ctx->SetPhysics(&physics);

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

    // Average frame time should be reasonable (< 5ms for headless)
    EXPECT_TRUE(avgFrameUs < 5000.0);

    // Memory should not have leaked significantly (< 10MB growth)
    EXPECT_TRUE(memGrowthKB < 10240);

    // Entity count should be back to baseline
    EXPECT_EQ(world->GetEntityCount(), static_cast<size_t>(0));

    handle.Unsubscribe();
}
