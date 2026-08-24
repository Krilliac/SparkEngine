/**
 * @file TestDebugHookManager.cpp
 * @brief Unit tests for Spark::DebugHookManager
 *
 * Tests registration, dispatch, priority ordering, RAII handles,
 * convenience methods, enable/disable toggle, name-based cleanup,
 * and multi-system lifecycle tracing patterns.
 */

#include "TestFramework.h"
#include "Utils/DebugHookManager.h"
#include "Utils/Process.h"

#include <algorithm>
#include <string>
#include <vector>

// ============================================================================
// Helper: reset the singleton between tests
// ============================================================================

static void ResetHookManager()
{
    auto& mgr = Spark::DebugHookManager::GetInstance();
    mgr.Clear();
    mgr.SetEnabled(true);
    mgr.SetFrameNumber(0);
    mgr.SetDeltaTime(0.0f);
}

// ============================================================================
// Registration and handler count
// ============================================================================

TEST(DebugHookManager_RegisterAndCount)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    EXPECT_EQ(mgr.GetTotalHandlerCount(), 0);
    EXPECT_FALSE(mgr.HasHandlers(Spark::DebugHookPoint::FrameBegin));

    auto handle = mgr.Register(Spark::DebugHookPoint::FrameBegin, "Test", [](const Spark::DebugHookContext&) {});

    EXPECT_TRUE(handle.IsActive());
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameBegin), 1);
    EXPECT_EQ(mgr.GetTotalHandlerCount(), 1);
    EXPECT_TRUE(mgr.HasHandlers(Spark::DebugHookPoint::FrameBegin));
}

TEST(DebugHookManager_RegisterMultiplePoints)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    auto h1 = mgr.Register(Spark::DebugHookPoint::FrameBegin, "A", [](const Spark::DebugHookContext&) {});
    auto h2 = mgr.Register(Spark::DebugHookPoint::FrameEnd, "B", [](const Spark::DebugHookContext&) {});
    auto h3 = mgr.Register(Spark::DebugHookPoint::FrameBegin, "C", [](const Spark::DebugHookContext&) {});

    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameBegin), 2);
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameEnd), 1);
    EXPECT_EQ(mgr.GetTotalHandlerCount(), 3);
}

// ============================================================================
// Dispatch invokes handlers
// ============================================================================

TEST(DebugHookManager_DispatchInvokesHandler)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    int callCount = 0;

    auto handle = mgr.Register(Spark::DebugHookPoint::FrameBegin, "Counter",
                               [&](const Spark::DebugHookContext&) { callCount++; });

    mgr.Dispatch(Spark::DebugHookPoint::FrameBegin);
    EXPECT_EQ(callCount, 1);

    mgr.Dispatch(Spark::DebugHookPoint::FrameBegin);
    EXPECT_EQ(callCount, 2);
}

TEST(DebugHookManager_DispatchNoHandlers)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    // Should not crash when dispatching with no handlers
    EXPECT_NO_THROW(mgr.Dispatch(Spark::DebugHookPoint::EnginePreInit));
}

TEST(DebugHookManager_DispatchWrongPoint)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    int callCount = 0;

    auto handle =
        mgr.Register(Spark::DebugHookPoint::FrameBegin, "Test", [&](const Spark::DebugHookContext&) { callCount++; });

    // Dispatching a different point should not invoke the handler
    mgr.Dispatch(Spark::DebugHookPoint::FrameEnd);
    EXPECT_EQ(callCount, 0);
}

// ============================================================================
// Context data passed correctly
// ============================================================================

TEST(DebugHookManager_ContextFrameInfo)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    uint64_t receivedFrame = 0;
    float receivedDt = 0.0f;

    auto handle = mgr.Register(Spark::DebugHookPoint::FrameBegin, "FrameCheck",
                               [&](const Spark::DebugHookContext& ctx)
                               {
                                   receivedFrame = ctx.frameNumber;
                                   receivedDt = ctx.deltaTime;
                               });

    mgr.Dispatch(Spark::DebugHookPoint::FrameBegin, 42, 0.016f);
    EXPECT_EQ(receivedFrame, static_cast<uint64_t>(42));
    EXPECT_GT(receivedDt, 0.015f);
    EXPECT_LT(receivedDt, 0.017f);
}

TEST(DebugHookManager_ContextSystemName)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    std::string receivedSystem;
    double receivedDuration = 0.0;

    auto handle = mgr.Register(Spark::DebugHookPoint::SystemPostUpdate, "SysCheck",
                               [&](const Spark::DebugHookContext& ctx)
                               {
                                   receivedSystem = std::string(ctx.systemName);
                                   receivedDuration = ctx.durationMs;
                               });

    mgr.DispatchSystem(Spark::DebugHookPoint::SystemPostUpdate, "Physics", 2.5);
    EXPECT_EQ(receivedSystem, std::string("Physics"));
    EXPECT_GT(receivedDuration, 2.4);
    EXPECT_LT(receivedDuration, 2.6);
}

TEST(DebugHookManager_ContextResourceName)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    std::string receivedResource;

    auto handle =
        mgr.Register(Spark::DebugHookPoint::ResourceLoadBegin, "ResCheck",
                     [&](const Spark::DebugHookContext& ctx) { receivedResource = std::string(ctx.resourceName); });

    mgr.DispatchResource(Spark::DebugHookPoint::ResourceLoadBegin, "textures/hero.png");
    EXPECT_EQ(receivedResource, std::string("textures/hero.png"));
}

TEST(DebugHookManager_ContextSceneName)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    std::string receivedScene;

    auto handle = mgr.Register(Spark::DebugHookPoint::ScenePreLoad, "SceneCheck",
                               [&](const Spark::DebugHookContext& ctx) { receivedScene = std::string(ctx.sceneName); });

    mgr.DispatchScene(Spark::DebugHookPoint::ScenePreLoad, "Level01");
    EXPECT_EQ(receivedScene, std::string("Level01"));
}

TEST(DebugHookManager_ContextMessage)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    std::string receivedMsg;

    auto handle = mgr.Register(Spark::DebugHookPoint::ErrorRaised, "ErrCheck",
                               [&](const Spark::DebugHookContext& ctx) { receivedMsg = std::string(ctx.message); });

    mgr.DispatchDebugMessage(Spark::DebugHookPoint::ErrorRaised, "Out of memory");
    EXPECT_EQ(receivedMsg, std::string("Out of memory"));
}

// ============================================================================
// Priority ordering
// ============================================================================

TEST(DebugHookManager_PriorityOrdering)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    std::vector<std::string> order;

    auto h1 = mgr.Register(
        Spark::DebugHookPoint::FrameBegin, "Low", [&](const Spark::DebugHookContext&) { order.push_back("low"); }, 100);
    auto h2 = mgr.Register(
        Spark::DebugHookPoint::FrameBegin, "High", [&](const Spark::DebugHookContext&) { order.push_back("high"); }, 1);
    auto h3 = mgr.Register(
        Spark::DebugHookPoint::FrameBegin, "Med", [&](const Spark::DebugHookContext&) { order.push_back("med"); }, 50);

    mgr.Dispatch(Spark::DebugHookPoint::FrameBegin);

    EXPECT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], std::string("high"));
    EXPECT_EQ(order[1], std::string("med"));
    EXPECT_EQ(order[2], std::string("low"));
}

// ============================================================================
// RAII handle auto-unregistration
// ============================================================================

TEST(DebugHookManager_RAIIHandle)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    {
        auto handle = mgr.Register(Spark::DebugHookPoint::FrameEnd, "Temporary", [](const Spark::DebugHookContext&) {});
        EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameEnd), 1);
    }

    // Handle destroyed — should have auto-unregistered
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameEnd), 0);
}

TEST(DebugHookManager_HandleMove)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    auto h1 = mgr.Register(Spark::DebugHookPoint::FrameEnd, "Movable", [](const Spark::DebugHookContext&) {});
    EXPECT_TRUE(h1.IsActive());

    auto h2 = std::move(h1);
    EXPECT_FALSE(h1.IsActive()); // NOLINT — testing moved-from state
    EXPECT_TRUE(h2.IsActive());
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameEnd), 1);
}

TEST(DebugHookManager_HandleManualUnregister)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    auto handle =
        mgr.Register(Spark::DebugHookPoint::EnginePreShutdown, "Manual", [](const Spark::DebugHookContext&) {});
    EXPECT_TRUE(handle.IsActive());

    handle.Unregister();
    EXPECT_FALSE(handle.IsActive());
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::EnginePreShutdown), 0);

    // Double unregister should be safe
    handle.Unregister();
}

// ============================================================================
// UnregisterAllByName
// ============================================================================

TEST(DebugHookManager_UnregisterAllByName)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    auto h1 = mgr.Register(Spark::DebugHookPoint::FrameBegin, "PhysicsDebug", [](const Spark::DebugHookContext&) {});
    auto h2 = mgr.Register(Spark::DebugHookPoint::FrameEnd, "PhysicsDebug", [](const Spark::DebugHookContext&) {});
    auto h3 = mgr.Register(Spark::DebugHookPoint::FrameBegin, "AudioDebug", [](const Spark::DebugHookContext&) {});

    EXPECT_EQ(mgr.GetTotalHandlerCount(), 3);

    mgr.UnregisterAllByName("PhysicsDebug");

    EXPECT_EQ(mgr.GetTotalHandlerCount(), 1);
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameBegin), 1);
    EXPECT_EQ(mgr.GetHandlerCount(Spark::DebugHookPoint::FrameEnd), 0);
}

// ============================================================================
// Enable/disable toggle
// ============================================================================

TEST(DebugHookManager_DisabledNoDispatch)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    int callCount = 0;

    auto handle = mgr.Register(Spark::DebugHookPoint::FrameBegin, "Counter",
                               [&](const Spark::DebugHookContext&) { callCount++; });

    mgr.SetEnabled(false);
    mgr.Dispatch(Spark::DebugHookPoint::FrameBegin);
    EXPECT_EQ(callCount, 0);

    mgr.SetEnabled(true);
    mgr.Dispatch(Spark::DebugHookPoint::FrameBegin);
    EXPECT_EQ(callCount, 1);
}

// ============================================================================
// Frame number and delta time propagation
// ============================================================================

TEST(DebugHookManager_StoredFrameInfo)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    mgr.SetFrameNumber(100);
    mgr.SetDeltaTime(0.033f);

    EXPECT_EQ(mgr.GetFrameNumber(), static_cast<uint64_t>(100));
    EXPECT_GT(mgr.GetDeltaTime(), 0.032f);

    // Dispatch without explicit frame info should use stored values
    uint64_t receivedFrame = 0;
    auto handle = mgr.Register(Spark::DebugHookPoint::SystemPreUpdate, "Check",
                               [&](const Spark::DebugHookContext& ctx) { receivedFrame = ctx.frameNumber; });

    mgr.DispatchSystem(Spark::DebugHookPoint::SystemPreUpdate, "Test");
    EXPECT_EQ(receivedFrame, static_cast<uint64_t>(100));
}

// ============================================================================
// Clear removes all hooks
// ============================================================================

TEST(DebugHookManager_Clear)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    auto h1 = mgr.Register(Spark::DebugHookPoint::FrameBegin, "A", [](const Spark::DebugHookContext&) {});
    auto h2 = mgr.Register(Spark::DebugHookPoint::FrameEnd, "B", [](const Spark::DebugHookContext&) {});

    EXPECT_EQ(mgr.GetTotalHandlerCount(), 2);

    mgr.Clear();
    EXPECT_EQ(mgr.GetTotalHandlerCount(), 0);
    EXPECT_FALSE(mgr.HasHandlers(Spark::DebugHookPoint::FrameBegin));
    EXPECT_FALSE(mgr.HasHandlers(Spark::DebugHookPoint::FrameEnd));
}

// ============================================================================
// DebugHookPointToString
// ============================================================================

TEST(DebugHookManager_PointToString)
{
    EXPECT_EQ(Spark::DebugHookPointToString(Spark::DebugHookPoint::FrameBegin), std::string_view("FrameBegin"));
    EXPECT_EQ(Spark::DebugHookPointToString(Spark::DebugHookPoint::SystemPostUpdate),
              std::string_view("SystemPostUpdate"));
    EXPECT_EQ(Spark::DebugHookPointToString(Spark::DebugHookPoint::ErrorRaised), std::string_view("ErrorRaised"));
}

// ============================================================================
// Macros compile and dispatch correctly
// ============================================================================

TEST(DebugHookManager_MacroDispatch)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    int callCount = 0;

    auto handle = mgr.Register(Spark::DebugHookPoint::FrameBegin, "MacroTest",
                               [&](const Spark::DebugHookContext&) { callCount++; });

    SPARK_DEBUG_HOOK(FrameBegin, 1, 0.016f);
    EXPECT_EQ(callCount, 1);

    // System macro
    int sysCount = 0;
    auto h2 = mgr.Register(Spark::DebugHookPoint::SystemPreUpdate, "SysMacro",
                           [&](const Spark::DebugHookContext&) { sysCount++; });

    SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Physics", 0.0);
    EXPECT_EQ(sysCount, 1);
}

// ============================================================================
// Multi-system lifecycle tracing
// ============================================================================

TEST(DebugHookManager_SystemLifecycleTrace)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    std::vector<std::string> trace;

    // Register a single handler that observes all system lifecycle events
    auto h1 = mgr.Register(Spark::DebugHookPoint::SystemPreInit, "Lifecycle", [&](const Spark::DebugHookContext& ctx)
                           { trace.push_back("PreInit:" + std::string(ctx.systemName)); });
    auto h2 = mgr.Register(Spark::DebugHookPoint::SystemPostInit, "Lifecycle", [&](const Spark::DebugHookContext& ctx)
                           { trace.push_back("PostInit:" + std::string(ctx.systemName)); });
    auto h3 =
        mgr.Register(Spark::DebugHookPoint::SystemPreShutdown, "Lifecycle", [&](const Spark::DebugHookContext& ctx)
                     { trace.push_back("PreShutdown:" + std::string(ctx.systemName)); });
    auto h4 =
        mgr.Register(Spark::DebugHookPoint::SystemPostShutdown, "Lifecycle", [&](const Spark::DebugHookContext& ctx)
                     { trace.push_back("PostShutdown:" + std::string(ctx.systemName)); });

    // Simulate a system lifecycle: Physics init, Audio init, Audio shutdown, Physics shutdown
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Physics", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Physics", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Audio", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Audio", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Audio", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Audio", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Physics", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Physics", 0.0);

    EXPECT_EQ(trace.size(), 8u);
    EXPECT_EQ(trace[0], std::string("PreInit:Physics"));
    EXPECT_EQ(trace[1], std::string("PostInit:Physics"));
    EXPECT_EQ(trace[2], std::string("PreInit:Audio"));
    EXPECT_EQ(trace[3], std::string("PostInit:Audio"));
    EXPECT_EQ(trace[4], std::string("PreShutdown:Audio"));
    EXPECT_EQ(trace[5], std::string("PostShutdown:Audio"));
    EXPECT_EQ(trace[6], std::string("PreShutdown:Physics"));
    EXPECT_EQ(trace[7], std::string("PostShutdown:Physics"));
}

// ============================================================================
// Per-system update timing pattern
// ============================================================================

TEST(DebugHookManager_SystemUpdateTimingPattern)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    struct SystemTiming
    {
        std::string name;
        double durationMs = 0.0;
    };
    std::vector<SystemTiming> timings;

    auto handle =
        mgr.Register(Spark::DebugHookPoint::SystemPostUpdate, "TimingCollector", [&](const Spark::DebugHookContext& ctx)
                     { timings.push_back({std::string(ctx.systemName), ctx.durationMs}); });

    // Simulate several systems reporting their update duration
    mgr.DispatchSystem(Spark::DebugHookPoint::SystemPostUpdate, "ECS.Physics", 3.2);
    mgr.DispatchSystem(Spark::DebugHookPoint::SystemPostUpdate, "ECS.Animation", 1.1);
    mgr.DispatchSystem(Spark::DebugHookPoint::SystemPostUpdate, "ECS.AI", 0.5);
    mgr.DispatchSystem(Spark::DebugHookPoint::SystemPostUpdate, "ECS.Render", 8.7);

    EXPECT_EQ(timings.size(), 4u);
    EXPECT_EQ(timings[0].name, std::string("ECS.Physics"));
    EXPECT_GT(timings[0].durationMs, 3.1);
    EXPECT_EQ(timings[3].name, std::string("ECS.Render"));
    EXPECT_GT(timings[3].durationMs, 8.6);
}

// ============================================================================
// Error/warning hook observability
// ============================================================================

TEST(DebugHookManager_ErrorWarningHooks)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    auto h1 = mgr.Register(Spark::DebugHookPoint::ErrorRaised, "ErrCollector",
                           [&](const Spark::DebugHookContext& ctx) { errors.push_back(std::string(ctx.message)); });
    auto h2 = mgr.Register(Spark::DebugHookPoint::WarningRaised, "WarnCollector",
                           [&](const Spark::DebugHookContext& ctx) { warnings.push_back(std::string(ctx.message)); });

    SPARK_DEBUG_HOOK_MESSAGE(ErrorRaised, "GPU device lost");
    SPARK_DEBUG_HOOK_MESSAGE(WarningRaised, "Texture pool 80% full");
    SPARK_DEBUG_HOOK_MESSAGE(ErrorRaised, "Shader compilation failed");

    EXPECT_EQ(errors.size(), 2u);
    EXPECT_EQ(warnings.size(), 1u);
    EXPECT_EQ(errors[0], std::string("GPU device lost"));
    EXPECT_EQ(warnings[0], std::string("Texture pool 80% full"));
}

// ============================================================================
// Resource load tracking
// ============================================================================

TEST(DebugHookManager_ResourceLoadTracking)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    struct ResourceEvent
    {
        std::string pointName;
        std::string name;
        double durationMs;
    };
    std::vector<ResourceEvent> events;

    auto h1 = mgr.Register(Spark::DebugHookPoint::ResourceLoadBegin, "ResTracker",
                           [&](const Spark::DebugHookContext& ctx)
                           {
                               events.push_back({std::string(Spark::DebugHookPointToString(ctx.point)),
                                                 std::string(ctx.resourceName), ctx.durationMs});
                           });
    auto h2 = mgr.Register(Spark::DebugHookPoint::ResourceLoadComplete, "ResTracker",
                           [&](const Spark::DebugHookContext& ctx)
                           {
                               events.push_back({std::string(Spark::DebugHookPointToString(ctx.point)),
                                                 std::string(ctx.resourceName), ctx.durationMs});
                           });

    SPARK_DEBUG_HOOK_RESOURCE(ResourceLoadBegin, "materials/brick.sparkmat", 0.0);
    SPARK_DEBUG_HOOK_RESOURCE(ResourceLoadComplete, "materials/brick.sparkmat", 12.3);
    SPARK_DEBUG_HOOK_RESOURCE(ResourceLoadBegin, "scripts/player.as", 0.0);
    SPARK_DEBUG_HOOK_RESOURCE(ResourceLoadComplete, "scripts/player.as", 2.1);

    EXPECT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].pointName, std::string("ResourceLoadBegin"));
    EXPECT_EQ(events[0].name, std::string("materials/brick.sparkmat"));
    EXPECT_EQ(events[1].pointName, std::string("ResourceLoadComplete"));
    EXPECT_GT(events[1].durationMs, 12.0);
    EXPECT_EQ(events[2].name, std::string("scripts/player.as"));
}

// ============================================================================
// Scene transition hooks
// ============================================================================

TEST(DebugHookManager_SceneTransitionHooks)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    std::vector<std::pair<std::string, std::string>> transitions;

    auto h1 = mgr.Register(Spark::DebugHookPoint::ScenePreLoad, "SceneTracker", [&](const Spark::DebugHookContext& ctx)
                           { transitions.push_back({"PreLoad", std::string(ctx.sceneName)}); });
    auto h2 = mgr.Register(Spark::DebugHookPoint::ScenePostLoad, "SceneTracker", [&](const Spark::DebugHookContext& ctx)
                           { transitions.push_back({"PostLoad", std::string(ctx.sceneName)}); });
    auto h3 =
        mgr.Register(Spark::DebugHookPoint::ScenePreUnload, "SceneTracker", [&](const Spark::DebugHookContext& ctx)
                     { transitions.push_back({"PreUnload", std::string(ctx.sceneName)}); });
    auto h4 =
        mgr.Register(Spark::DebugHookPoint::ScenePostUnload, "SceneTracker", [&](const Spark::DebugHookContext& ctx)
                     { transitions.push_back({"PostUnload", std::string(ctx.sceneName)}); });

    SPARK_DEBUG_HOOK_SCENE(ScenePreLoad, "Level01");
    SPARK_DEBUG_HOOK_SCENE(ScenePostLoad, "Level01");
    SPARK_DEBUG_HOOK_SCENE(ScenePreUnload, "Level01");
    SPARK_DEBUG_HOOK_SCENE(ScenePostUnload, "Level01");

    EXPECT_EQ(transitions.size(), 4u);
    EXPECT_EQ(transitions[0].first, std::string("PreLoad"));
    EXPECT_EQ(transitions[0].second, std::string("Level01"));
    EXPECT_EQ(transitions[3].first, std::string("PostUnload"));
}

// ============================================================================
// Mixed hook types in single frame simulation
// ============================================================================

TEST(DebugHookManager_FullFrameSimulation)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();
    mgr.SetFrameNumber(42);
    mgr.SetDeltaTime(0.016f);

    std::vector<std::string> events;

    auto h1 = mgr.Register(Spark::DebugHookPoint::FrameBegin, "FrameTracer", [&](const Spark::DebugHookContext& ctx)
                           { events.push_back("FrameBegin:" + std::to_string(ctx.frameNumber)); });
    auto h2 =
        mgr.Register(Spark::DebugHookPoint::SystemPreUpdate, "FrameTracer", [&](const Spark::DebugHookContext& ctx)
                     { events.push_back("PreUpdate:" + std::string(ctx.systemName)); });
    auto h3 =
        mgr.Register(Spark::DebugHookPoint::SystemPostUpdate, "FrameTracer", [&](const Spark::DebugHookContext& ctx)
                     { events.push_back("PostUpdate:" + std::string(ctx.systemName)); });
    auto h4 = mgr.Register(Spark::DebugHookPoint::FrameEnd, "FrameTracer", [&](const Spark::DebugHookContext& ctx)
                           { events.push_back("FrameEnd:" + std::to_string(ctx.frameNumber)); });

    // Simulate a full frame
    SPARK_DEBUG_HOOK(FrameBegin, 42, 0.016f);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Physics", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Physics", 2.1);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "ECS.Render", 0.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.Render", 5.3);
    SPARK_DEBUG_HOOK(FrameEnd, 42, 0.016f);

    EXPECT_EQ(events.size(), 6u);
    EXPECT_EQ(events[0], std::string("FrameBegin:42"));
    EXPECT_EQ(events[1], std::string("PreUpdate:Physics"));
    EXPECT_EQ(events[2], std::string("PostUpdate:Physics"));
    EXPECT_EQ(events[5], std::string("FrameEnd:42"));
}

// ============================================================================
// Selective system filtering
// ============================================================================

TEST(DebugHookManager_SelectiveSystemFilter)
{
    ResetHookManager();
    auto& mgr = Spark::DebugHookManager::GetInstance();

    // Only track Physics updates, ignore everything else
    int physicsUpdates = 0;
    auto handle = mgr.Register(Spark::DebugHookPoint::SystemPostUpdate, "PhysicsOnly",
                               [&](const Spark::DebugHookContext& ctx)
                               {
                                   if (ctx.systemName == "Physics")
                                       physicsUpdates++;
                               });

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Physics", 2.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Audio", 0.5);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "ECS.AI", 1.0);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Physics", 2.1);

    EXPECT_EQ(physicsUpdates, 2);
}

TEST(DebugHookManager_ProcessTeardownAfterNetworkInitIsClean)
{
#ifdef SPARK_TEST_DEBUG_HOOK_TEARDOWN_PROBE_PATH
    auto probe = Spark::Process::Builder(SPARK_TEST_DEBUG_HOOK_TEARDOWN_PROBE_PATH).Launch();
    ASSERT_TRUE(probe.has_value());
    EXPECT_EQ(probe->WaitForExit(), 0);
#else
    EXPECT_TRUE(false);
#endif
}
