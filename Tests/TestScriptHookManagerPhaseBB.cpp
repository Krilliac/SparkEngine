/**
 * @file TestScriptHookManagerPhaseBB.cpp
 * @brief Phase BB Theme 3D tests for Spark::Scripting::ScriptHookManager
 *
 * The existing Tests/TestScriptHookManager.cpp is yet another
 * local-reimplementation anti-pattern (see Phase X). It defines its
 * own `HookType` / `HookContext` / `HookHandler` types in an anonymous
 * namespace and never touches the real class in
 * `Engine/Scripting/ScriptHookManager.h`.
 *
 * Phase BB ships tests against the real class, wired in
 * `GameplayLifecycleShared.cpp` as a startup touch + shutdown `Clear()`:
 *
 *   - Singleton accessor stability
 *   - RegisterHook returns monotonically-increasing IDs
 *   - DispatchHook runs handlers in priority order (low → high)
 *   - Handlers can cancel / modify the context
 *   - UnregisterHook by ID
 *   - UnregisterAllForScript removes only the named script's hooks
 *   - Clear resets every hook
 *   - GetTotalHandlerCount / HasHandlers / per-type handler count
 *   - DispatchCombatHook / DispatchEntityHook convenience wrappers
 *   - Thread safety — concurrent registers and dispatches
 */

#include "TestFramework.h"
#include "Engine/Scripting/ScriptHookManager.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace
{

    // Each test starts with a clean hook table so prior tests cannot
    // leak handlers into this run.
    void ResetHookManager()
    {
        Spark::Scripting::ScriptHookManager::GetInstance().Clear();
    }

} // namespace

// ============================================================================
// Singleton
// ============================================================================

TEST(ScriptHookManagerPhaseBB_SingletonReturnsSameInstance)
{
    auto& a = Spark::Scripting::ScriptHookManager::GetInstance();
    auto& b = Spark::Scripting::ScriptHookManager::GetInstance();
    EXPECT_TRUE(&a == &b);
}

// ============================================================================
// RegisterHook / DispatchHook basics
// ============================================================================

TEST(ScriptHookManagerPhaseBB_RegisterHookReturnsNonZeroId)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();
    uint32_t id = hooks.RegisterHook(
        Spark::Scripting::HookType::OnDamageDealt, "TestScript", [](Spark::Scripting::HookContext&) {}, 0);
    EXPECT_TRUE(id > 0);
    ResetHookManager();
}

TEST(ScriptHookManagerPhaseBB_DispatchRunsRegisteredHandler)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    int callCount = 0;
    hooks.RegisterHook(
        Spark::Scripting::HookType::OnEntityCreated, "CountScript",
        [&](Spark::Scripting::HookContext&) { ++callCount; }, 0);

    Spark::Scripting::HookContext ctx{};
    ctx.type = Spark::Scripting::HookType::OnEntityCreated;
    hooks.DispatchHook(Spark::Scripting::HookType::OnEntityCreated, ctx);

    EXPECT_EQ(callCount, 1);
    ResetHookManager();
}

TEST(ScriptHookManagerPhaseBB_DispatchNoHandlersIsSafe)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();
    Spark::Scripting::HookContext ctx{};
    ctx.type = Spark::Scripting::HookType::OnKill;
    // Must not crash when no handlers are registered.
    hooks.DispatchHook(Spark::Scripting::HookType::OnKill, ctx);
    EXPECT_EQ(hooks.GetTotalHandlerCount(), 0);
    ResetHookManager();
}

// ============================================================================
// Priority ordering — low priority runs first
// ============================================================================

TEST(ScriptHookManagerPhaseBB_PriorityOrdering)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    std::vector<int> order;
    hooks.RegisterHook(
        Spark::Scripting::HookType::OnAbilityCast, "Late", [&](Spark::Scripting::HookContext&) { order.push_back(30); },
        30);
    hooks.RegisterHook(
        Spark::Scripting::HookType::OnAbilityCast, "First", [&](Spark::Scripting::HookContext&) { order.push_back(0); },
        0);
    hooks.RegisterHook(
        Spark::Scripting::HookType::OnAbilityCast, "Mid", [&](Spark::Scripting::HookContext&) { order.push_back(10); },
        10);

    Spark::Scripting::HookContext ctx{};
    ctx.type = Spark::Scripting::HookType::OnAbilityCast;
    hooks.DispatchHook(Spark::Scripting::HookType::OnAbilityCast, ctx);

    EXPECT_EQ(order.size(), static_cast<size_t>(3));
    EXPECT_EQ(order[0], 0);
    EXPECT_EQ(order[1], 10);
    EXPECT_EQ(order[2], 30);

    ResetHookManager();
}

// ============================================================================
// Context modification (cancellation + value override)
// ============================================================================

TEST(ScriptHookManagerPhaseBB_HandlerCanCancel)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    hooks.RegisterHook(
        Spark::Scripting::HookType::OnAbilityCast, "CancelScript",
        [](Spark::Scripting::HookContext& ctx) { ctx.cancelled = true; }, 0);

    Spark::Scripting::HookContext ctx{};
    ctx.type = Spark::Scripting::HookType::OnAbilityCast;
    auto result = hooks.DispatchHook(Spark::Scripting::HookType::OnAbilityCast, ctx);

    EXPECT_TRUE(result.cancelled);
    ResetHookManager();
}

TEST(ScriptHookManagerPhaseBB_HandlerCanModifyValue)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    hooks.RegisterHook(
        Spark::Scripting::HookType::OnDamageDealt, "ArmorScript",
        [](Spark::Scripting::HookContext& ctx)
        {
            ctx.modifiedValue = ctx.floatParam1 * 0.8f; // 20% reduction
            ctx.valueModified = true;
        },
        0);

    auto result = hooks.DispatchCombatHook(Spark::Scripting::HookType::OnDamageDealt, 1, 2, 100.0f, 0);

    EXPECT_TRUE(result.valueModified);
    EXPECT_NEAR(result.modifiedValue, 80.0f, 0.001f);
    ResetHookManager();
}

// ============================================================================
// UnregisterHook
// ============================================================================

TEST(ScriptHookManagerPhaseBB_UnregisterHookById)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    int called = 0;
    uint32_t id = hooks.RegisterHook(
        Spark::Scripting::HookType::OnHeal, "HealScript", [&](Spark::Scripting::HookContext&) { ++called; }, 0);

    EXPECT_EQ(hooks.GetTotalHandlerCount(), 1);

    hooks.UnregisterHook(id);
    EXPECT_EQ(hooks.GetTotalHandlerCount(), 0);

    Spark::Scripting::HookContext ctx{};
    hooks.DispatchHook(Spark::Scripting::HookType::OnHeal, ctx);
    EXPECT_EQ(called, 0); // Handler was unregistered, should not fire.

    ResetHookManager();
}

TEST(ScriptHookManagerPhaseBB_UnregisterAllForScript)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    hooks.RegisterHook(Spark::Scripting::HookType::OnDamageDealt, "ScriptA", [](Spark::Scripting::HookContext&) {}, 0);
    hooks.RegisterHook(Spark::Scripting::HookType::OnHeal, "ScriptA", [](Spark::Scripting::HookContext&) {}, 0);
    hooks.RegisterHook(Spark::Scripting::HookType::OnKill, "ScriptB", [](Spark::Scripting::HookContext&) {}, 0);

    EXPECT_EQ(hooks.GetTotalHandlerCount(), 3);

    hooks.UnregisterAllForScript("ScriptA");
    EXPECT_EQ(hooks.GetTotalHandlerCount(), 1);

    // ScriptB's hook on OnKill should still be alive.
    EXPECT_TRUE(hooks.HasHandlers(Spark::Scripting::HookType::OnKill));
    EXPECT_FALSE(hooks.HasHandlers(Spark::Scripting::HookType::OnDamageDealt));
    EXPECT_FALSE(hooks.HasHandlers(Spark::Scripting::HookType::OnHeal));

    ResetHookManager();
}

// ============================================================================
// Clear
// ============================================================================

TEST(ScriptHookManagerPhaseBB_ClearRemovesAllHandlers)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    for (int i = 0; i < 5; ++i)
    {
        hooks.RegisterHook(
            Spark::Scripting::HookType::OnDamageDealt, "Script_" + std::to_string(i),
            [](Spark::Scripting::HookContext&) {}, i);
    }
    EXPECT_EQ(hooks.GetTotalHandlerCount(), 5);

    hooks.Clear();
    EXPECT_EQ(hooks.GetTotalHandlerCount(), 0);
    EXPECT_FALSE(hooks.HasHandlers(Spark::Scripting::HookType::OnDamageDealt));

    ResetHookManager();
}

// ============================================================================
// Convenience dispatchers
// ============================================================================

TEST(ScriptHookManagerPhaseBB_DispatchCombatHookFillsContext)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    Spark::Scripting::HookContext captured{};
    hooks.RegisterHook(
        Spark::Scripting::HookType::OnDamageDealt, "CaptureScript",
        [&](Spark::Scripting::HookContext& ctx) { captured = ctx; }, 0);

    hooks.DispatchCombatHook(Spark::Scripting::HookType::OnDamageDealt, /*source=*/101, /*target=*/202,
                             /*value=*/42.5f, /*school=*/3);

    EXPECT_EQ(captured.sourceEntity, static_cast<Spark::Scripting::EntityID>(101));
    EXPECT_EQ(captured.targetEntity, static_cast<Spark::Scripting::EntityID>(202));
    EXPECT_NEAR(captured.floatParam1, 42.5f, 0.001f);
    EXPECT_EQ(captured.param1, static_cast<uint32_t>(3));

    ResetHookManager();
}

TEST(ScriptHookManagerPhaseBB_DispatchEntityHookFillsContext)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    Spark::Scripting::HookContext captured{};
    hooks.RegisterHook(
        Spark::Scripting::HookType::OnEntityDestroyed, "DestroyScript",
        [&](Spark::Scripting::HookContext& ctx) { captured = ctx; }, 0);

    hooks.DispatchEntityHook(Spark::Scripting::HookType::OnEntityDestroyed, /*entity=*/999, /*param=*/7);

    EXPECT_EQ(captured.sourceEntity, static_cast<Spark::Scripting::EntityID>(999));
    EXPECT_EQ(captured.param1, static_cast<uint32_t>(7));

    ResetHookManager();
}

// ============================================================================
// Handler counts
// ============================================================================

TEST(ScriptHookManagerPhaseBB_HasHandlersPerType)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    hooks.RegisterHook(Spark::Scripting::HookType::OnDamageDealt, "S1", [](Spark::Scripting::HookContext&) {}, 0);

    EXPECT_TRUE(hooks.HasHandlers(Spark::Scripting::HookType::OnDamageDealt));
    EXPECT_FALSE(hooks.HasHandlers(Spark::Scripting::HookType::OnHeal));

    ResetHookManager();
}

// ============================================================================
// Thread safety — concurrent register + dispatch
// ============================================================================

TEST(ScriptHookManagerPhaseBB_ConcurrentRegisterDispatch)
{
    ResetHookManager();
    auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();

    std::atomic<int> totalFires{0};
    std::atomic<bool> stop{false};

    // Dispatcher thread
    std::thread dispatcher(
        [&]()
        {
            while (!stop.load())
            {
                Spark::Scripting::HookContext ctx{};
                ctx.type = Spark::Scripting::HookType::OnDamageDealt;
                hooks.DispatchHook(Spark::Scripting::HookType::OnDamageDealt, ctx);
            }
        });

    // Registrar thread — registers and unregisters 50 hooks.
    std::thread registrar(
        [&]()
        {
            std::vector<uint32_t> ids;
            for (int i = 0; i < 50; ++i)
            {
                uint32_t id = hooks.RegisterHook(
                    Spark::Scripting::HookType::OnDamageDealt, "RaceScript_" + std::to_string(i),
                    [&](Spark::Scripting::HookContext&) { totalFires.fetch_add(1); }, 0);
                ids.push_back(id);
            }
            for (uint32_t id : ids)
                hooks.UnregisterHook(id);
        });

    registrar.join();
    stop.store(true);
    dispatcher.join();

    // After unregister, dispatch should be safe and handler count is zero.
    EXPECT_EQ(hooks.GetTotalHandlerCount(), 0);
    ResetHookManager();
}
