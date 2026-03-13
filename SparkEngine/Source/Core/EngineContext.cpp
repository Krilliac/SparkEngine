/**
 * @file EngineContext.cpp
 * @brief Concrete IEngineContext implementation
 *
 * R1.1: Constructor now delegates to RegisterSystem<T> for each subsystem.
 * R1.2: InitializeAll() / ShutdownAll() with topological sort.
 */

#include "EngineContext.h"
#include "Spark/Version.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

// Global engine context - defined here (in SparkEngineLib) so that all
// consumers of the static library (both the executable and SparkGame DLL)
// can resolve this symbol at link time.
std::unique_ptr<EngineContext> g_engineContext;

EngineContext* EngineContext::Get()
{
    return g_engineContext.get();
}

const std::unique_ptr<EngineContext>& EngineContext::GetOwned()
{
    return g_engineContext;
}

void EngineContext::SetOwned(std::unique_ptr<EngineContext> ctx)
{
    g_engineContext = std::move(ctx);
}

void EngineContext::ResetOwned()
{
    g_engineContext.reset();
}

EngineContext::EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer, Spark::EventBus* eventBus)
{
    // Delegate all storage to the generic registry (R1.1)
    if (graphics)
    {
        RegisterSystem<GraphicsEngine>(graphics);
    }
    if (input)
    {
        RegisterSystem<InputManager>(input);
    }
    if (timer)
    {
        RegisterSystem<Timer>(timer);
    }
    if (eventBus)
    {
        RegisterSystem<Spark::EventBus>(eventBus);
    }
}

uint32_t EngineContext::GetEngineVersion() const
{
    return Spark::GetEngineVersion();
}

uint32_t EngineContext::GetSDKVersion() const
{
    return Spark::GetSDKVersion();
}

#ifdef SPARK_HEADLESS_SUPPORT
bool g_headlessMode = false;
#endif

bool EngineContext::IsHeadless() const
{
#ifdef SPARK_HEADLESS_SUPPORT
    return g_headlessMode;
#else
    return false;
#endif
}

// =============================================================================
// Dependency-aware subsystem lifecycle (R1.2)
// =============================================================================

bool EngineContext::TopologicalSort(std::vector<SubsystemEntry*>& sorted)
{
    // Build adjacency + in-degree from m_subsystemEntries
    std::unordered_map<TypeId, SubsystemEntry*, TypeIdHash> entryMap;
    std::unordered_map<TypeId, int, TypeIdHash> inDegree;
    std::unordered_map<TypeId, std::vector<TypeId>, TypeIdHash> adjacency; // dep -> dependents

    for (auto& entry : m_subsystemEntries)
    {
        entryMap[entry.type] = &entry;
        inDegree[entry.type]; // ensure key exists with default 0
    }

    for (auto& entry : m_subsystemEntries)
    {
        for (const auto& dep : entry.dependencies)
        {
            // Only count edges where the dependency is also a registered subsystem
            if (entryMap.find(dep) != entryMap.end())
            {
                adjacency[dep].push_back(entry.type);
                inDegree[entry.type]++;
            }
        }
    }

    // Kahn's algorithm
    std::vector<TypeId> queue;
    for (const auto& [type, degree] : inDegree)
    {
        if (degree == 0)
        {
            queue.push_back(type);
        }
    }

    // Sort the initial queue for deterministic ordering
    std::sort(queue.begin(), queue.end(), [](TypeId a, TypeId b) { return std::less<const void*>{}(a, b); });

    sorted.clear();
    size_t idx = 0;

    while (idx < queue.size())
    {
        auto current = queue[idx++];
        sorted.push_back(entryMap[current]);

        // Collect and sort neighbors for determinism
        auto adjIt = adjacency.find(current);
        if (adjIt != adjacency.end())
        {
            auto neighbors = adjIt->second;
            std::sort(neighbors.begin(), neighbors.end(),
                      [](TypeId a, TypeId b) { return std::less<const void*>{}(a, b); });

            for (const auto& neighbor : neighbors)
            {
                inDegree[neighbor]--;
                if (inDegree[neighbor] == 0)
                {
                    queue.push_back(neighbor);
                }
            }
        }
    }

    // If we didn't visit all entries, there's a cycle
    return sorted.size() == m_subsystemEntries.size();
}

bool EngineContext::InitializeAll()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "EngineContext::InitializeAll — %zu subsystems registered",
                   m_subsystemEntries.size());

    std::vector<SubsystemEntry*> sorted;
    if (!TopologicalSort(sorted))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext: dependency cycle detected in subsystem graph");
        return false;
    }

    m_initOrder.clear();

    for (auto* entry : sorted)
    {
        if (entry->initFn)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Core, "EngineContext: initializing subsystem (type=%p)", entry->type);
            if (!entry->initFn())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext: subsystem initialization failed (type=%p)",
                                entry->type);
                return false;
            }
        }
        entry->initialized = true;
        m_initOrder.push_back(entry->type);
    }

    SPARK_LOG_INFO(Spark::LogCategory::Core, "EngineContext::InitializeAll — all %zu subsystems initialized",
                   m_initOrder.size());
    return true;
}

void EngineContext::ShutdownAll()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "EngineContext::ShutdownAll — shutting down %zu subsystems",
                   m_initOrder.size());

    // Shut down in reverse initialization order
    for (auto it = m_initOrder.rbegin(); it != m_initOrder.rend(); ++it)
    {
        auto entryIt = std::find_if(m_subsystemEntries.begin(), m_subsystemEntries.end(),
                                    [&](const SubsystemEntry& e) { return e.type == *it; });
        if (entryIt != m_subsystemEntries.end() && entryIt->initialized && entryIt->shutdownFn)
        {
            entryIt->shutdownFn();
        }
        if (entryIt != m_subsystemEntries.end())
        {
            entryIt->initialized = false;
        }
    }

    m_initOrder.clear();
}
