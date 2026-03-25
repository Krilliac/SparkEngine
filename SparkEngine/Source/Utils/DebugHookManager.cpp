/**
 * @file DebugHookManager.cpp
 * @brief Implementation of the project-wide debugging hook system
 */

#include "DebugHookManager.h"
#include "Validate.h"

#include <algorithm>

namespace Spark
{

    // =============================================================================
    // DebugHookHandle
    // =============================================================================

    void DebugHookHandle::Unregister()
    {
        if (m_manager && m_id != 0)
        {
            m_manager->Unregister(m_id);
            m_manager = nullptr;
            m_id = 0;
        }
    }

    // =============================================================================
    // DebugHookManager — Singleton
    // =============================================================================

    DebugHookManager& DebugHookManager::GetInstance()
    {
        static DebugHookManager instance;
        return instance;
    }

    // =============================================================================
    // Registration
    // =============================================================================

    DebugHookHandle DebugHookManager::Register(DebugHookPoint point, const std::string& name, DebugHookHandler handler,
                                               int priority)
    {
        uint32_t id = m_nextId.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& list = m_hooks[point];
            list.push_back({id, point, name, std::move(handler), priority});

            // Keep sorted by priority (stable: equal priorities preserve insertion order)
            std::stable_sort(list.begin(), list.end(),
                             [](const HookEntry& a, const HookEntry& b) { return a.priority < b.priority; });
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "DebugHookManager::Register id=%u name='%s' point=%d priority=%d", id,
                        name.c_str(), static_cast<int>(point), priority);
        return DebugHookHandle(this, id);
    }

    void DebugHookManager::Unregister(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [point, list] : m_hooks)
        {
            auto it = std::remove_if(list.begin(), list.end(), [id](const HookEntry& e) { return e.id == id; });
            if (it != list.end())
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Core, "DebugHookManager::Unregister id=%u", id);
                list.erase(it, list.end());
                return;
            }
        }
    }

    void DebugHookManager::UnregisterAllByName(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [point, list] : m_hooks)
        {
            list.erase(std::remove_if(list.begin(), list.end(), [&name](const HookEntry& e) { return e.name == name; }),
                       list.end());
        }
    }

    // =============================================================================
    // Dispatch
    // =============================================================================

    void DebugHookManager::Dispatch(const DebugHookContext& context)
    {
        if (!m_enabled)
            return;

        // Snapshot handlers under lock, then invoke without lock
        std::vector<DebugHookHandler> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_hooks.find(context.point);
            if (it == m_hooks.end() || it->second.empty())
                return;

            snapshot.reserve(it->second.size());
            for (const auto& entry : it->second)
            {
                snapshot.push_back(entry.handler);
            }
        }

        for (const auto& handler : snapshot)
        {
            handler(context);
        }
    }

    void DebugHookManager::Dispatch(DebugHookPoint point, uint64_t frameNumber, float deltaTime)
    {
        if (!m_enabled)
            return;

        DebugHookContext ctx{};
        ctx.point = point;
        ctx.frameNumber = (frameNumber != 0) ? frameNumber : m_frameNumber.load();
        ctx.deltaTime = (deltaTime != 0.0f) ? deltaTime : m_deltaTime.load();
        Dispatch(ctx);
    }

    void DebugHookManager::DispatchSystem(DebugHookPoint point, std::string_view systemName, double durationMs)
    {
        if (!m_enabled)
            return;

        DebugHookContext ctx{};
        ctx.point = point;
        ctx.frameNumber = m_frameNumber.load();
        ctx.deltaTime = m_deltaTime.load();
        ctx.systemName = systemName;
        ctx.durationMs = durationMs;
        Dispatch(ctx);
    }

    void DebugHookManager::DispatchResource(DebugHookPoint point, std::string_view resourceName, double durationMs)
    {
        if (!m_enabled)
            return;

        DebugHookContext ctx{};
        ctx.point = point;
        ctx.frameNumber = m_frameNumber.load();
        ctx.deltaTime = m_deltaTime.load();
        ctx.resourceName = resourceName;
        ctx.durationMs = durationMs;
        Dispatch(ctx);
    }

    void DebugHookManager::DispatchScene(DebugHookPoint point, std::string_view sceneName)
    {
        if (!m_enabled)
            return;

        DebugHookContext ctx{};
        ctx.point = point;
        ctx.frameNumber = m_frameNumber.load();
        ctx.deltaTime = m_deltaTime.load();
        ctx.sceneName = sceneName;
        Dispatch(ctx);
    }

    void DebugHookManager::DispatchDebugMessage(DebugHookPoint point, std::string_view message)
    {
        if (!m_enabled)
            return;

        DebugHookContext ctx{};
        ctx.point = point;
        ctx.frameNumber = m_frameNumber.load();
        ctx.deltaTime = m_deltaTime.load();
        ctx.message = message;
        Dispatch(ctx);
    }

    // =============================================================================
    // Queries
    // =============================================================================

    bool DebugHookManager::HasHandlers(DebugHookPoint point) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hooks.find(point);
        return it != m_hooks.end() && !it->second.empty();
    }

    int DebugHookManager::GetHandlerCount(DebugHookPoint point) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hooks.find(point);
        if (it == m_hooks.end())
            return 0;
        return static_cast<int>(it->second.size());
    }

    int DebugHookManager::GetTotalHandlerCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        int total = 0;
        for (const auto& [point, list] : m_hooks)
        {
            total += static_cast<int>(list.size());
        }
        return total;
    }

    void DebugHookManager::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hooks.clear();
    }

} // namespace Spark
