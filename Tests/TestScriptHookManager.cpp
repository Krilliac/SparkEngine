// TestScriptHookManager.cpp - Tests for Spark::Scripting::ScriptHookManager
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation of Spark::Scripting::ScriptHookManager
// ============================================================================

namespace
{

    enum class HookType : uint32_t
    {
        OnPlayerSpawn,
        OnPlayerDeath,
        OnItemPickup,
        OnLevelLoad,
        OnChatMessage
    };

    class HookContext
    {
      public:
        explicit HookContext(HookType type) : m_type(type) {}

        HookType GetType() const { return m_type; }

        void Cancel() { m_cancelled = true; }
        bool IsCancelled() const { return m_cancelled; }

        void SetValue(const std::string& key, const std::string& value) { m_values[key] = value; }

        std::string GetValue(const std::string& key) const
        {
            auto it = m_values.find(key);
            if (it != m_values.end())
                return it->second;
            return "";
        }

        bool HasValue(const std::string& key) const { return m_values.find(key) != m_values.end(); }

      private:
        HookType m_type;
        bool m_cancelled = false;
        std::unordered_map<std::string, std::string> m_values;
    };

    using HookHandler = std::function<void(HookContext&)>;

    struct HookRegistration
    {
        uint32_t id = 0;
        std::string scriptName;
        int priority = 0; // Lower number = higher priority (runs first)
        HookHandler handler;
    };

    class ScriptHookManager
    {
      public:
        uint32_t RegisterHook(HookType type, const std::string& scriptName, int priority, HookHandler handler)
        {
            uint32_t id = m_nextId++;
            m_hooks[type].push_back({id, scriptName, priority, std::move(handler)});

            // Sort by priority (lower = higher priority = runs first)
            auto& handlers = m_hooks[type];
            std::sort(handlers.begin(), handlers.end(),
                      [](const HookRegistration& a, const HookRegistration& b) { return a.priority < b.priority; });

            return id;
        }

        bool UnregisterHook(uint32_t hookId)
        {
            for (auto& [type, handlers] : m_hooks)
            {
                for (auto it = handlers.begin(); it != handlers.end(); ++it)
                {
                    if (it->id == hookId)
                    {
                        handlers.erase(it);
                        return true;
                    }
                }
            }
            return false;
        }

        size_t UnregisterAllForScript(const std::string& scriptName)
        {
            size_t removed = 0;
            for (auto& [type, handlers] : m_hooks)
            {
                auto newEnd = std::remove_if(handlers.begin(), handlers.end(),
                                             [&](const HookRegistration& r) { return r.scriptName == scriptName; });
                removed += static_cast<size_t>(std::distance(newEnd, handlers.end()));
                handlers.erase(newEnd, handlers.end());
            }
            return removed;
        }

        void Dispatch(HookContext& context) const
        {
            auto it = m_hooks.find(context.GetType());
            if (it == m_hooks.end())
                return;

            for (const auto& reg : it->second)
            {
                reg.handler(context);
                if (context.IsCancelled())
                    break;
            }
        }

        bool HasHandlers(HookType type) const
        {
            auto it = m_hooks.find(type);
            return it != m_hooks.end() && !it->second.empty();
        }

        size_t GetHandlerCount(HookType type) const
        {
            auto it = m_hooks.find(type);
            if (it == m_hooks.end())
                return 0;
            return it->second.size();
        }

      private:
        uint32_t m_nextId = 1;
        std::unordered_map<HookType, std::vector<HookRegistration>> m_hooks;
    };

} // namespace

// ============================================================================
// Register hook and verify handler count
// ============================================================================

TEST(ScriptHookManager_RegisterHook)
{
    ScriptHookManager manager;

    auto id = manager.RegisterHook(HookType::OnPlayerSpawn, "TestScript", 0, [](HookContext&) {});
    EXPECT_GT(id, 0u);
    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerSpawn), 1u);
}

TEST(ScriptHookManager_RegisterMultiple)
{
    ScriptHookManager manager;

    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptA", 0, [](HookContext&) {});
    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptB", 0, [](HookContext&) {});
    manager.RegisterHook(HookType::OnPlayerDeath, "ScriptA", 0, [](HookContext&) {});

    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerSpawn), 2u);
    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerDeath), 1u);
}

// ============================================================================
// Dispatch hook invokes handler
// ============================================================================

TEST(ScriptHookManager_DispatchInvokesHandler)
{
    ScriptHookManager manager;
    int callCount = 0;

    manager.RegisterHook(HookType::OnItemPickup, "TestScript", 0, [&](HookContext&) { callCount++; });

    HookContext ctx(HookType::OnItemPickup);
    manager.Dispatch(ctx);

    EXPECT_EQ(callCount, 1);
}

TEST(ScriptHookManager_DispatchNoHandlers)
{
    ScriptHookManager manager;

    HookContext ctx(HookType::OnLevelLoad);
    EXPECT_NO_THROW(manager.Dispatch(ctx));
}

// ============================================================================
// Priority ordering (lower number = higher priority = runs first)
// ============================================================================

TEST(ScriptHookManager_PriorityOrdering)
{
    ScriptHookManager manager;
    std::vector<std::string> executionOrder;

    manager.RegisterHook(HookType::OnPlayerSpawn, "LowPri", 100,
                         [&](HookContext&) { executionOrder.push_back("low"); });
    manager.RegisterHook(HookType::OnPlayerSpawn, "HighPri", 1,
                         [&](HookContext&) { executionOrder.push_back("high"); });
    manager.RegisterHook(HookType::OnPlayerSpawn, "MedPri", 50, [&](HookContext&) { executionOrder.push_back("med"); });

    HookContext ctx(HookType::OnPlayerSpawn);
    manager.Dispatch(ctx);

    EXPECT_EQ(executionOrder.size(), 3u);
    EXPECT_EQ(executionOrder[0], std::string("high"));
    EXPECT_EQ(executionOrder[1], std::string("med"));
    EXPECT_EQ(executionOrder[2], std::string("low"));
}

// ============================================================================
// Unregister specific hook
// ============================================================================

TEST(ScriptHookManager_UnregisterHook)
{
    ScriptHookManager manager;

    auto id = manager.RegisterHook(HookType::OnPlayerDeath, "TestScript", 0, [](HookContext&) {});
    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerDeath), 1u);

    bool removed = manager.UnregisterHook(id);
    EXPECT_TRUE(removed);
    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerDeath), 0u);
}

TEST(ScriptHookManager_UnregisterNonexistent)
{
    ScriptHookManager manager;

    bool removed = manager.UnregisterHook(9999);
    EXPECT_FALSE(removed);
}

// ============================================================================
// UnregisterAllForScript
// ============================================================================

TEST(ScriptHookManager_UnregisterAllForScript)
{
    ScriptHookManager manager;

    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptA", 0, [](HookContext&) {});
    manager.RegisterHook(HookType::OnPlayerDeath, "ScriptA", 0, [](HookContext&) {});
    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptB", 0, [](HookContext&) {});

    size_t removed = manager.UnregisterAllForScript("ScriptA");
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerSpawn), 1u);
    EXPECT_EQ(manager.GetHandlerCount(HookType::OnPlayerDeath), 0u);
}

TEST(ScriptHookManager_UnregisterAllForScript_NoMatches)
{
    ScriptHookManager manager;

    manager.RegisterHook(HookType::OnItemPickup, "ScriptA", 0, [](HookContext&) {});

    size_t removed = manager.UnregisterAllForScript("NonexistentScript");
    EXPECT_EQ(removed, 0u);
}

// ============================================================================
// HookContext cancellation
// ============================================================================

TEST(ScriptHookManager_Cancellation)
{
    ScriptHookManager manager;
    int secondHandlerCalls = 0;

    manager.RegisterHook(HookType::OnChatMessage, "Filter", 1, [](HookContext& ctx) { ctx.Cancel(); });
    manager.RegisterHook(HookType::OnChatMessage, "Logger", 100, [&](HookContext&) { secondHandlerCalls++; });

    HookContext ctx(HookType::OnChatMessage);
    manager.Dispatch(ctx);

    EXPECT_TRUE(ctx.IsCancelled());
    EXPECT_EQ(secondHandlerCalls, 0); // Second handler should not run
}

TEST(ScriptHookManager_NoCancellation)
{
    ScriptHookManager manager;
    int totalCalls = 0;

    manager.RegisterHook(HookType::OnChatMessage, "ScriptA", 1, [&](HookContext&) { totalCalls++; });
    manager.RegisterHook(HookType::OnChatMessage, "ScriptB", 2, [&](HookContext&) { totalCalls++; });

    HookContext ctx(HookType::OnChatMessage);
    manager.Dispatch(ctx);

    EXPECT_FALSE(ctx.IsCancelled());
    EXPECT_EQ(totalCalls, 2);
}

// ============================================================================
// HookContext value modification
// ============================================================================

TEST(ScriptHookManager_ContextValues)
{
    HookContext ctx(HookType::OnChatMessage);

    EXPECT_FALSE(ctx.HasValue("message"));
    EXPECT_EQ(ctx.GetValue("message"), std::string(""));

    ctx.SetValue("message", "Hello, world!");
    EXPECT_TRUE(ctx.HasValue("message"));
    EXPECT_EQ(ctx.GetValue("message"), std::string("Hello, world!"));
}

TEST(ScriptHookManager_ContextValueModifiedByHandler)
{
    ScriptHookManager manager;

    manager.RegisterHook(HookType::OnChatMessage, "Censor", 1,
                         [](HookContext& ctx) { ctx.SetValue("message", "[censored]"); });

    HookContext ctx(HookType::OnChatMessage);
    ctx.SetValue("message", "bad word");
    manager.Dispatch(ctx);

    EXPECT_EQ(ctx.GetValue("message"), std::string("[censored]"));
}

// ============================================================================
// HasHandlers check
// ============================================================================

TEST(ScriptHookManager_HasHandlers)
{
    ScriptHookManager manager;

    EXPECT_FALSE(manager.HasHandlers(HookType::OnLevelLoad));

    manager.RegisterHook(HookType::OnLevelLoad, "TestScript", 0, [](HookContext&) {});
    EXPECT_TRUE(manager.HasHandlers(HookType::OnLevelLoad));
    EXPECT_FALSE(manager.HasHandlers(HookType::OnPlayerDeath));
}

// ============================================================================
// Multiple handlers same hook type
// ============================================================================

TEST(ScriptHookManager_MultipleHandlersSameHook)
{
    ScriptHookManager manager;
    std::string accumulated;

    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptA", 1, [&](HookContext&) { accumulated += "A"; });
    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptB", 2, [&](HookContext&) { accumulated += "B"; });
    manager.RegisterHook(HookType::OnPlayerSpawn, "ScriptC", 3, [&](HookContext&) { accumulated += "C"; });

    HookContext ctx(HookType::OnPlayerSpawn);
    manager.Dispatch(ctx);

    EXPECT_EQ(accumulated, std::string("ABC"));
}
