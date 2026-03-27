/**
 * @file TestEngineDiagnostics.cpp
 * @brief Runs the runtime diagnostics against live singleton subsystems.
 *
 * Each subsystem diagnostic gracefully handles missing/uninitialized services
 * (reports them as "System available: FAIL") so this test validates that
 * the diagnostic framework itself works and exercises every reachable path.
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Core/EngineDiagnostics.h"
#include "../SparkEngine/Source/Core/EngineContext.h"

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

#include <iostream>
#include <memory>

// ============================================================================
// Helper: Initialize a minimal EngineContext with singleton subsystems
// so the diagnostics can find them via EngineContext::Get()
// ============================================================================

static void SetupMinimalContext()
{
    // Create context if it doesn't exist
    if (!EngineContext::Get())
    {
        EngineContext::SetOwned(std::make_unique<EngineContext>());
    }

    auto* ctx = EngineContext::Get();

    // Register singleton subsystems that are available
    static Spark::EventBus eventBus;
    ctx->SetEventBus(&eventBus);

    static World world;
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

    // TweenSystem needs Initialize()
    auto& tweens = Spark::TweenSystem::GetInstance();
    tweens.Initialize();
    ctx->SetTween(&tweens);

    // SaveSystem needs Initialize()
    auto& save = Spark::SaveSystem::GetInstance();
    save.Initialize();
    ctx->SetSaveSystem(&save);

    // VFS
    auto& vfs = Spark::VirtualFileSystem::GetInstance();
    vfs.Initialize();
    ctx->SetVFS(&vfs);

    // FileCache
    static Spark::LocalFileCache fileCache;
    ctx->SetFileCache(&fileCache);

    // Physics
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

    // Network: DiagNetworking uses Spark::Net::NetworkManager::GetInstance() directly,
    // so no need to register via EngineContext (type mismatch: Spark::NetworkManager vs Spark::Net::NetworkManager)
}

// ============================================================================
// Full diag_all test
// ============================================================================

TEST(DiagAll_RunsAllSubsystems)
{
    SetupMinimalContext();

    Spark::DiagReport report;

    // Run all diagnostics
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
    Spark::DiagPhysics(report);
    Spark::DiagNetworking(report);

    // Print the full report
    std::string output = report.Format();
    std::cout << output << std::flush;

    // At minimum, we should have run many checks
    EXPECT_TRUE(report.results.size() > 20);

    // The "System available" check should pass for every subsystem we registered
    int systemAvailablePassed = 0;
    for (const auto& r : report.results)
    {
        if (r.testName == "System available" && r.passed)
            systemAvailablePassed++;
    }
    EXPECT_TRUE(systemAvailablePassed >= 10);
}

// Individual subsystem smoke tests
TEST(DiagEventBus_Standalone)
{
    Spark::DiagReport report;
    Spark::DiagEventBus(report);
    EXPECT_TRUE(report.failed == 0);
    EXPECT_TRUE(report.passed >= 5);
}

TEST(DiagMemory_Standalone)
{
    Spark::DiagReport report;
    Spark::DiagMemory(report);
    EXPECT_TRUE(report.failed == 0);
}

TEST(DiagReport_FormatNotEmpty)
{
    Spark::DiagReport report;
    report.Add("Test", "check1", true, "ok");
    report.Add("Test", "check2", false, "bad");
    auto str = report.Format();
    EXPECT_TRUE(str.find("1 passed") != std::string::npos);
    EXPECT_TRUE(str.find("1 failed") != std::string::npos);
    EXPECT_TRUE(str.find("PASS") != std::string::npos);
    EXPECT_TRUE(str.find("FAIL") != std::string::npos);
}
