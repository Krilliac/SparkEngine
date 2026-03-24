/**
 * @file TestDebugHookManager.cpp
 * @brief Unit tests for Spark::DebugHookManager
 *
 * Tests registration, dispatch, priority ordering, RAII handles,
 * convenience methods, enable/disable toggle, and name-based cleanup.
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/DebugHookManager.h"

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

    mgr.DispatchMessage(Spark::DebugHookPoint::ErrorRaised, "Out of memory");
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
