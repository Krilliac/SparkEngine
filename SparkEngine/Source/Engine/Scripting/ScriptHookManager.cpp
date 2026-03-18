/**
 * @file ScriptHookManager.cpp
 * @brief Implementation of the centralized script hook dispatcher
 */

#include "ScriptHookManager.h"
#include <algorithm>

namespace Spark::Scripting
{

    // =============================================================================
    // Singleton
    // =============================================================================

    ScriptHookManager& ScriptHookManager::GetInstance()
    {
        static ScriptHookManager instance;
        return instance;
    }

    // =============================================================================
    // Registration
    // =============================================================================

    uint32_t ScriptHookManager::RegisterHook(HookType type, const std::string& scriptName, HookHandler handler,
                                             int priority)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32_t id = m_nextId++;

        HookRegistration reg;
        reg.id = id;
        reg.scriptName = scriptName;
        reg.handler = std::move(handler);
        reg.priority = priority;

        auto& handlers = m_hooks[type];
        handlers.push_back(std::move(reg));

        // Keep sorted by priority (lowest first) for dispatch order
        std::sort(handlers.begin(), handlers.end(),
                  [](const HookRegistration& a, const HookRegistration& b) { return a.priority < b.priority; });

        return id;
    }

    void ScriptHookManager::UnregisterHook(uint32_t registrationId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& [type, handlers] : m_hooks)
        {
            auto it = std::find_if(handlers.begin(), handlers.end(),
                                   [registrationId](const HookRegistration& reg) { return reg.id == registrationId; });

            if (it != handlers.end())
            {
                handlers.erase(it);
                return;
            }
        }
    }

    void ScriptHookManager::UnregisterAllForScript(const std::string& scriptName)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& [type, handlers] : m_hooks)
        {
            std::erase_if(handlers,
                          [&scriptName](const HookRegistration& reg) { return reg.scriptName == scriptName; });
        }
    }

    // =============================================================================
    // Dispatch
    // =============================================================================

    HookContext ScriptHookManager::DispatchHook(HookType type, HookContext context)
    {
        context.type = type;

        // Copy handlers under lock, then release before invoking.
        // This prevents deadlocks if a handler calls back into the manager.
        std::vector<HookRegistration> localHandlers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_hooks.find(type);
            if (it == m_hooks.end() || it->second.empty())
            {
                return context;
            }
            localHandlers = it->second;
        }

        for (const auto& reg : localHandlers)
        {
            reg.handler(context);
        }

        return context;
    }

    HookContext ScriptHookManager::DispatchCombatHook(HookType type, EntityID source, EntityID target, float value,
                                                      uint32_t school)
    {
        HookContext ctx;
        ctx.sourceEntity = source;
        ctx.targetEntity = target;
        ctx.floatParam1 = value;
        ctx.param1 = school;
        ctx.modifiedValue = value; // Default to unmodified
        return DispatchHook(type, ctx);
    }

    HookContext ScriptHookManager::DispatchEntityHook(HookType type, EntityID entity, uint32_t param)
    {
        HookContext ctx;
        ctx.sourceEntity = entity;
        ctx.param1 = param;
        return DispatchHook(type, ctx);
    }

    // =============================================================================
    // Query
    // =============================================================================

    bool ScriptHookManager::HasHandlers(HookType type) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hooks.find(type);
        return it != m_hooks.end() && !it->second.empty();
    }

    int ScriptHookManager::GetTotalHandlerCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        int total = 0;
        for (const auto& [type, handlers] : m_hooks)
        {
            total += static_cast<int>(handlers.size());
        }
        return total;
    }

    int ScriptHookManager::GetHandlerCount(HookType type) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hooks.find(type);
        if (it == m_hooks.end())
        {
            return 0;
        }
        return static_cast<int>(it->second.size());
    }

    void ScriptHookManager::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hooks.clear();
    }

} // namespace Spark::Scripting
