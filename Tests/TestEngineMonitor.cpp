/**
 * @file TestEngineMonitor.cpp
 * @brief Extended engine monitoring — runs subsystems for many frames,
 *        measures frame times, detects spikes, checks memory health,
 *        and validates system consistency over time.
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
#include "../SparkEngine/Source/Utils/EventBus.h"
#include "../SparkEngine/Source/Utils/FrameInspector.h"
#include "../SparkEngine/Source/Utils/LocalFileCache.h"
#include "../SparkEngine/Source/Utils/MemoryMonitor.h"
#include "../SparkEngine/Source/Utils/Profiler.h"
#include "../SparkEngine/Source/Utils/Timer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

// ============================================================================
// Minimal engine context setup (shared with TestEngineDiagnostics)
// ============================================================================

static World* g_testWorld = nullptr;
static Spark::EventBus* g_testEventBus = nullptr;

static void EnsureMinimalContext()
{
    if (!EngineContext::Get())
    {
        EngineContext::SetOwned(std::make_unique<EngineContext>());
    }

    auto* ctx = EngineContext::Get();

    static Spark::EventBus eventBus;
    g_testEventBus = &eventBus;
    ctx->SetEventBus(&eventBus);

    static World world;
    g_testWorld = &world;
    ctx->SetWorld(&world);

    static Spark::WeatherSystem weather;
    ctx->SetWeather(&weather);

    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());
    ctx->SetAbilities(&Spark::Gameplay::AbilitySystem::GetInstance());
    ctx->SetConditions(&Spark::Gameplay::ConditionSystem::GetInstance());
    ctx->SetInstances(&Spark::Gameplay::InstanceManager::GetInstance());

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
}

// ============================================================================
// Frame timing stats
// ============================================================================

struct FrameStats
{
    std::vector<double> frameTimes; // microseconds
    double minUs = 1e9;
    double maxUs = 0.0;
    double totalUs = 0.0;
    int spikeCount = 0;   // frames > 3x average
    int severeSpikes = 0; // frames > 10x average
    double spikeThresholdUs = 0.0;

    void Record(double us)
    {
        frameTimes.push_back(us);
        minUs = std::min(minUs, us);
        maxUs = std::max(maxUs, us);
        totalUs += us;
    }

    void Analyze()
    {
        if (frameTimes.empty())
            return;
        double avgUs = totalUs / static_cast<double>(frameTimes.size());
        spikeThresholdUs = avgUs * 3.0;
        for (double t : frameTimes)
        {
            if (t > avgUs * 3.0)
                spikeCount++;
            if (t > avgUs * 10.0)
                severeSpikes++;
        }
    }

    double AverageUs() const { return frameTimes.empty() ? 0.0 : totalUs / static_cast<double>(frameTimes.size()); }

    double P95Us() const
    {
        if (frameTimes.empty())
            return 0.0;
        auto sorted = frameTimes;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(static_cast<double>(sorted.size()) * 0.95);
        if (idx >= sorted.size())
            idx = sorted.size() - 1;
        return sorted[idx];
    }

    double P99Us() const
    {
        if (frameTimes.empty())
            return 0.0;
        auto sorted = frameTimes;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(static_cast<double>(sorted.size()) * 0.99);
        if (idx >= sorted.size())
            idx = sorted.size() - 1;
        return sorted[idx];
    }

    double StdDevUs() const
    {
        if (frameTimes.size() < 2)
            return 0.0;
        double avg = AverageUs();
        double sumSq = 0.0;
        for (double t : frameTimes)
        {
            double d = t - avg;
            sumSq += d * d;
        }
        return std::sqrt(sumSq / static_cast<double>(frameTimes.size() - 1));
    }

    std::string Format() const
    {
        std::ostringstream ss;
        ss << "  Frames: " << frameTimes.size() << "\n";
        ss << "  Avg:    " << std::fixed << std::setprecision(1) << AverageUs() << " us (" << std::setprecision(1)
           << (AverageUs() / 1000.0) << " ms)\n";
        ss << "  Min:    " << std::setprecision(1) << minUs << " us\n";
        ss << "  Max:    " << std::setprecision(1) << maxUs << " us\n";
        ss << "  P95:    " << std::setprecision(1) << P95Us() << " us\n";
        ss << "  P99:    " << std::setprecision(1) << P99Us() << " us\n";
        ss << "  StdDev: " << std::setprecision(1) << StdDevUs() << " us\n";
        ss << "  Spikes (>3x avg): " << spikeCount << "\n";
        ss << "  Severe (>10x avg): " << severeSpikes << "\n";
        return ss.str();
    }
};

// ============================================================================
// Simulate one engine frame: tick all subsystems
// ============================================================================

static void SimulateFrame(float dt)
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
        return;

    // Fixed timestep accumulator
    Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

    // Weather
    auto* weather = ctx->GetWeather();
    if (weather)
        weather->Update(dt);

    // Time of day
    auto* tod = ctx->GetTimeOfDay();
    if (tod)
        tod->Update(dt);

    // Coroutines
    auto* coro = ctx->GetCoroutineScheduler();
    if (coro)
        coro->Update(dt);

    // Tweens
    auto* tween = ctx->GetTween();
    if (tween)
        tween->Update(dt);

    // Abilities
    auto* abilities = ctx->GetAbilities();
    if (abilities && ctx->GetWorld())
        abilities->Update(*ctx->GetWorld(), dt);

    // Instances
    auto* instances = ctx->GetInstances();
    if (instances)
        instances->Update(dt);

    // Profiler frame boundary
    Profiler::GetInstance().BeginFrame();
    Profiler::GetInstance().EndFrame();
}

// ============================================================================
// TEST: Extended frame monitoring (600 frames = ~10 seconds at 60 Hz)
// ============================================================================

TEST(Monitor_ExtendedFrameTiming)
{
    EnsureMinimalContext();

    constexpr int NUM_FRAMES = 600;
    constexpr float FRAME_DT = 1.0f / 60.0f;

    FrameStats stats;

    for (int i = 0; i < NUM_FRAMES; i++)
    {
        auto start = std::chrono::high_resolution_clock::now();
        SimulateFrame(FRAME_DT);
        auto end = std::chrono::high_resolution_clock::now();

        double us = std::chrono::duration<double, std::micro>(end - start).count();
        stats.Record(us);
    }

    stats.Analyze();

    std::cout << "\n=== Extended Frame Timing (600 frames) ===\n" << stats.Format() << std::flush;

    // Assertions: no severe spikes (>10x average)
    // CI runners with shared CPUs can have scheduling jitter — allow up to 10 spikes
    EXPECT_TRUE(stats.severeSpikes <= 10);
    // Average frame time should be reasonable (< 50ms for headless)
    EXPECT_TRUE(stats.AverageUs() < 50000.0);
}

// ============================================================================
// TEST: Memory stability over time
// ============================================================================

TEST(Monitor_MemoryStability)
{
    EnsureMinimalContext();

    auto& monitor = Spark::MemoryMonitor::GetInstance();
    auto baseline = monitor.TakeSnapshot();

    // Run 300 frames with entity churn
    constexpr int NUM_FRAMES = 300;
    constexpr float DT = 1.0f / 60.0f;
    auto* world = EngineContext::Get()->GetWorld();

    for (int i = 0; i < NUM_FRAMES; i++)
    {
        // Create and destroy entities to stress allocators
        if (world)
        {
            std::vector<EntityID> temp;
            for (int j = 0; j < 10; j++)
            {
                auto e = world->CreateEntity("__monitor_temp_" + std::to_string(j));
                world->AddComponent<HealthComponent>(e, HealthComponent{100.0f, 100.0f});
                world->AddComponent<Transform>(e, Transform{});
                temp.push_back(e);
            }
            // Destroy all
            for (auto e : temp)
                world->DestroyEntity(e);
        }

        SimulateFrame(DT);
    }

    auto afterChurn = monitor.TakeSnapshot();
    auto anomalies = monitor.GetRecentAnomalies();

    std::cout << "\n=== Memory Stability (300 frames, 3000 entity create/destroy cycles) ===\n";
    std::cout << "  Baseline:  " << baseline.totalBytes << " bytes, " << baseline.activeAllocations << " allocs\n";
    std::cout << "  After:     " << afterChurn.totalBytes << " bytes, " << afterChurn.activeAllocations << " allocs\n";

    int64_t byteDelta = static_cast<int64_t>(afterChurn.totalBytes) - static_cast<int64_t>(baseline.totalBytes);
    int64_t allocDelta =
        static_cast<int64_t>(afterChurn.activeAllocations) - static_cast<int64_t>(baseline.activeAllocations);

    std::cout << "  Delta:     " << byteDelta << " bytes, " << allocDelta << " allocs\n";
    std::cout << "  Anomalies: " << anomalies.size() << "\n";

    for (size_t i = 0; i < anomalies.size() && i < 10; i++)
    {
        std::cout << "    [" << static_cast<int>(anomalies[i].type) << "] " << anomalies[i].description << "\n";
    }
    std::cout << std::flush;

    // No severe anomalies
    int severeCount = 0;
    for (const auto& a : anomalies)
    {
        // DoubleFree or BudgetExceeded are severe
        if (a.type == Spark::MemoryAnomalyType::DoubleFree || a.type == Spark::MemoryAnomalyType::BudgetExceeded)
        {
            severeCount++;
        }
    }
    EXPECT_EQ(severeCount, 0);
}

// ============================================================================
// TEST: Weather system cycling stability
// ============================================================================

TEST(Monitor_WeatherCycling)
{
    EnsureMinimalContext();
    auto* weather = EngineContext::Get()->GetWeather();
    EXPECT_TRUE(weather != nullptr);
    if (!weather)
        return;

    constexpr float DT = 1.0f / 60.0f;
    FrameStats stats;

    // Cycle through all weather types 5 times with transitions
    const Spark::WeatherType types[] = {Spark::WeatherType::Clear, Spark::WeatherType::Rain, Spark::WeatherType::Snow,
                                        Spark::WeatherType::Storm, Spark::WeatherType::Fog};

    for (int cycle = 0; cycle < 5; cycle++)
    {
        for (auto wt : types)
        {
            weather->SetWeather(wt, 1.0f, 0.5f); // 0.5s transition

            // Run 30 frames per weather type (0.5s)
            for (int f = 0; f < 30; f++)
            {
                auto start = std::chrono::high_resolution_clock::now();
                weather->Update(DT);
                auto end = std::chrono::high_resolution_clock::now();
                double us = std::chrono::duration<double, std::micro>(end - start).count();
                stats.Record(us);
            }
        }
    }

    stats.Analyze();

    std::cout << "\n=== Weather Cycling (5 full cycles, 750 frames) ===\n" << stats.Format() << std::flush;

    // Weather updates should be fast (generous for Debug/ASan builds)
    EXPECT_TRUE(stats.AverageUs() < 10000.0); // < 10ms
    EXPECT_TRUE(stats.severeSpikes <= 5);

    // Final state should be valid
    auto finalState = weather->GetCurrentState();
    EXPECT_TRUE(finalState.intensity >= 0.0f && finalState.intensity <= 1.1f);
}

// ============================================================================
// TEST: TimeOfDay full day cycle
// ============================================================================

TEST(Monitor_TimeOfDayFullCycle)
{
    EnsureMinimalContext();
    auto* tod = EngineContext::Get()->GetTimeOfDay();
    EXPECT_TRUE(tod != nullptr);
    if (!tod)
        return;

    float originalTime = tod->GetTimeOfDay();
    float originalScale = tod->GetTimeScale();

    // Set fast time scale and run through a full 24h cycle
    tod->SetTimeOfDay(0.0f);
    tod->SetTimeScale(3600.0f); // 1 hour per second

    FrameStats stats;
    int intensityFlips = 0;
    bool wasDaytime = false;

    constexpr int FRAMES = 1440; // 24 seconds at 60Hz = 24 hours at 3600x
    constexpr float DT = 1.0f / 60.0f;

    for (int i = 0; i < FRAMES; i++)
    {
        auto start = std::chrono::high_resolution_clock::now();
        tod->Update(DT);
        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        stats.Record(us);

        float intensity = tod->GetSunIntensity();
        bool isDaytime = intensity > 0.5f;
        if (isDaytime != wasDaytime)
        {
            intensityFlips++;
            wasDaytime = isDaytime;
        }

        // Sun intensity should always be in valid range
        EXPECT_TRUE(intensity >= 0.0f && intensity <= 1.01f);
    }

    stats.Analyze();

    std::cout << "\n=== TimeOfDay Full 24h Cycle (1440 frames) ===\n" << stats.Format();
    std::cout << "  Day/Night transitions: " << intensityFlips << "\n" << std::flush;

    // Should have seen at least 1 day/night transition
    EXPECT_TRUE(intensityFlips >= 1);
    EXPECT_TRUE(stats.AverageUs() < 500.0); // < 0.5ms per update

    // Restore
    tod->SetTimeOfDay(originalTime);
    tod->SetTimeScale(originalScale);
}

// ============================================================================
// TEST: ECS entity churn stress (rapid create/destroy)
// ============================================================================

TEST(Monitor_ECSEntityChurn)
{
    EnsureMinimalContext();
    auto* world = EngineContext::Get()->GetWorld();
    EXPECT_TRUE(world != nullptr);
    if (!world)
        return;

    size_t initialCount = world->GetEntityCount();
    FrameStats createStats;
    FrameStats destroyStats;

    constexpr int ROUNDS = 100;
    constexpr int BATCH_SIZE = 50;

    for (int round = 0; round < ROUNDS; round++)
    {
        // Create batch
        std::vector<EntityID> batch;
        batch.reserve(BATCH_SIZE);

        auto createStart = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < BATCH_SIZE; i++)
        {
            auto e = world->CreateEntity();
            world->AddComponent<Transform>(e, Transform{});
            world->AddComponent<HealthComponent>(e, HealthComponent{100.0f, 100.0f});
            batch.push_back(e);
        }
        auto createEnd = std::chrono::high_resolution_clock::now();
        createStats.Record(std::chrono::duration<double, std::micro>(createEnd - createStart).count());

        // Verify count
        EXPECT_EQ(world->GetEntityCount(), initialCount + BATCH_SIZE);

        // Destroy batch
        auto destroyStart = std::chrono::high_resolution_clock::now();
        for (auto e : batch)
            world->DestroyEntity(e);
        auto destroyEnd = std::chrono::high_resolution_clock::now();
        destroyStats.Record(std::chrono::duration<double, std::micro>(destroyEnd - destroyStart).count());

        // Verify count restored
        EXPECT_EQ(world->GetEntityCount(), initialCount);
    }

    createStats.Analyze();
    destroyStats.Analyze();

    std::cout << "\n=== ECS Entity Churn (100 rounds x 50 entities) ===\n";
    std::cout << "Create (50 entities):\n" << createStats.Format();
    std::cout << "Destroy (50 entities):\n" << destroyStats.Format();
    std::cout << "  Total entities created/destroyed: " << (ROUNDS * BATCH_SIZE) << "\n";
    std::cout << "  Final entity count matches initial: " << (world->GetEntityCount() == initialCount ? "YES" : "NO")
              << "\n"
              << std::flush;

    // No severe spikes in entity operations
    EXPECT_TRUE(createStats.severeSpikes <= 2);
    EXPECT_TRUE(destroyStats.severeSpikes <= 2);
    // Final count must match
    EXPECT_EQ(world->GetEntityCount(), initialCount);
}

// ============================================================================
// TEST: EventBus throughput under load
// ============================================================================

TEST(Monitor_EventBusThroughput)
{
    Spark::EventBus bus;

    struct PerfEvent
    {
        int value;
    };

    int totalReceived = 0;

    // Register 10 subscribers
    std::vector<Spark::SubscriptionHandle> handles;
    for (int i = 0; i < 10; i++)
    {
        handles.push_back(bus.Subscribe<PerfEvent>([&](const PerfEvent&) { totalReceived++; }));
    }

    constexpr int NUM_PUBLISHES = 10000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_PUBLISHES; i++)
    {
        bus.Publish(PerfEvent{i});
    }
    auto end = std::chrono::high_resolution_clock::now();
    double totalUs = std::chrono::duration<double, std::micro>(end - start).count();
    double perPublishUs = totalUs / NUM_PUBLISHES;

    std::cout << "\n=== EventBus Throughput (10k publishes x 10 subscribers) ===\n";
    std::cout << "  Total time:       " << std::fixed << std::setprecision(0) << totalUs << " us\n";
    std::cout << "  Per publish:      " << std::setprecision(2) << perPublishUs << " us\n";
    std::cout << "  Events delivered: " << totalReceived << "\n" << std::flush;

    EXPECT_EQ(totalReceived, NUM_PUBLISHES * 10);
    // Each publish should be fast (< 100us for 10 subscribers)
    EXPECT_TRUE(perPublishUs < 100.0);
}

// ============================================================================
// TEST: Tween system sustained load
// ============================================================================

TEST(Monitor_TweenSustainedLoad)
{
    EnsureMinimalContext();
    auto* tweens = EngineContext::Get()->GetTween();
    EXPECT_TRUE(tweens != nullptr);
    if (!tweens)
        return;

    constexpr float DT = 1.0f / 60.0f;
    FrameStats stats;

    // Create 100 simultaneous tweens
    std::vector<float> values(100, 0.0f);
    for (int i = 0; i < 100; i++)
    {
        tweens->TweenFloat(values[i], 0.0f, 1.0f, 2.0f + static_cast<float>(i) * 0.01f, Spark::EaseType::EaseOutQuad);
    }

    EXPECT_TRUE(tweens->GetActiveTweenCount() >= 100);

    // Run for 180 frames (3 seconds)
    for (int f = 0; f < 180; f++)
    {
        auto start = std::chrono::high_resolution_clock::now();
        tweens->Update(DT);
        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        stats.Record(us);
    }

    stats.Analyze();

    // All tweens should have completed
    int completed = 0;
    for (float v : values)
    {
        if (v >= 0.99f)
            completed++;
    }

    std::cout << "\n=== Tween Sustained Load (100 simultaneous, 180 frames) ===\n" << stats.Format();
    std::cout << "  Completed tweens: " << completed << "/100\n" << std::flush;

    EXPECT_TRUE(completed == 100);
    EXPECT_TRUE(stats.AverageUs() < 2000.0); // < 2ms for 100 tweens
    EXPECT_TRUE(stats.severeSpikes == 0);
}

// ============================================================================
// TEST: Network server start/stop cycling
// ============================================================================

TEST(Monitor_NetworkServerCycling)
{
    auto& net = Spark::Net::NetworkManager::GetInstance();

    FrameStats stats;
    int successfulCycles = 0;

    // Start and stop server 10 times on different ports
    for (int i = 0; i < 10; i++)
    {
        uint16_t port = static_cast<uint16_t>(50000 + i);
        auto start = std::chrono::high_resolution_clock::now();

        bool started = net.StartServer(port, 4);
        if (started)
        {
            // Verify state
            EXPECT_TRUE(net.GetRole() == Spark::Net::NetworkRole::Server);

            // Tick a few frames
            for (int f = 0; f < 5; f++)
                net.Update(1.0f / 60.0f);

            net.StopServer();
            EXPECT_TRUE(net.GetRole() == Spark::Net::NetworkRole::None);
            successfulCycles++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        stats.Record(us);
    }

    stats.Analyze();

    std::cout << "\n=== Network Server Cycling (10 start/stop cycles) ===\n" << stats.Format();
    std::cout << "  Successful cycles: " << successfulCycles << "/10\n" << std::flush;

    EXPECT_TRUE(successfulCycles >= 8); // allow some port-in-use failures
}

// ============================================================================
// TEST: Coroutine scheduling throughput
// ============================================================================

TEST(Monitor_CoroutineScheduling)
{
    EnsureMinimalContext();
    auto& scheduler = Spark::CoroutineScheduler::GetInstance();

    int completedCount = 0;

    // Schedule 200 coroutines that increment a counter
    for (int i = 0; i < 200; i++)
    {
        scheduler.StartCoroutine("__monitor_coro_" + std::to_string(i)).Do([&]() { completedCount++; });
    }

    FrameStats stats;

    // Tick 60 frames to let them all complete
    for (int f = 0; f < 60; f++)
    {
        auto start = std::chrono::high_resolution_clock::now();
        scheduler.Update(1.0f / 60.0f);
        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        stats.Record(us);
    }

    stats.Analyze();

    std::cout << "\n=== Coroutine Scheduling (200 coroutines) ===\n" << stats.Format();
    std::cout << "  Completed: " << completedCount << "/200\n" << std::flush;

    EXPECT_EQ(completedCount, 200);
    EXPECT_TRUE(stats.AverageUs() < 5000.0); // < 5ms
}

// ============================================================================
// TEST: Combined monitoring summary
// ============================================================================

TEST(Monitor_CombinedSummary)
{
    EnsureMinimalContext();

    // Run diag_all and print results for a final health check
    Spark::DiagReport report;
    Spark::DiagEventBus(report);
    Spark::DiagMemory(report);
    Spark::DiagECS(report);
    Spark::DiagWeather(report);
    Spark::DiagTimeOfDay(report);
    Spark::DiagCoroutines(report);
    Spark::DiagConditions(report);
    Spark::DiagInstances(report);
    Spark::DiagTweens(report);
    Spark::DiagFileCache(report);
    Spark::DiagVFS(report);
    Spark::DiagAbilities(report);
    Spark::DiagDialogue(report);
    Spark::DiagSaveSystem(report);
    Spark::DiagNetworking(report);

    std::cout << "\n=== Final Health Check (after all monitoring tests) ===" << report.Format() << std::flush;

    // Count system failures
    int systemFailures = 0;
    for (const auto& r : report.results)
    {
        if (r.testName == "System available" && !r.passed)
            systemFailures++;
    }

    // Allow physics to be missing (not initialized in test context)
    EXPECT_TRUE(systemFailures <= 1);
    EXPECT_TRUE(report.passed >= 85);
}
