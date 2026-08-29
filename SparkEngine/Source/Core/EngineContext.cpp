/**
 * @file EngineContext.cpp
 * @brief Concrete IEngineContext implementation
 *
 * R1.1: Constructor now delegates to RegisterSystem<T> for each subsystem.
 * R1.2: InitializeAll() / ShutdownAll() with topological sort.
 */

#include "EngineContext.h"
#include "Spark/Version.h"
#include "../Utils/ContainerUtils.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <unordered_map>

namespace
{
    template <typename Emit> void BestEffortLifecycleDiagnostic(Emit&& emit) noexcept
    {
        try
        {
            emit();
        }
        catch (...)
        {
            std::fputs("[EngineContext] lifecycle diagnostic could not be emitted\n", stderr);
        }
    }
} // namespace

// Global engine context - defined here (in SparkEngineLib) so that all
// consumers of the static library (both the executable and SparkGame DLL)
// can resolve this symbol at link time.
std::unique_ptr<EngineContext> g_engineContext;

// Non-owning pointer to a host EngineContext injected across a DLL boundary.
// SparkEngineLib is statically linked into every module DLL, so g_engineContext
// is a per-image global that is null inside modules. When the host injects its
// live context via SetInjected(), Get() prefers it so module-side EngineContext::Get()
// resolves to the engine's real context instead of a dead per-image instance.
// It is deliberately NOT a unique_ptr — modules must never own/free the host context.
static EngineContext* g_injectedContext = nullptr;

EngineContext* EngineContext::Get()
{
    if (g_injectedContext)
    {
        return g_injectedContext;
    }
    return g_engineContext.get();
}

void EngineContext::SetInjected(EngineContext* ctx)
{
    g_injectedContext = ctx;
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
// NOTE: g_headlessMode is a plain bool because it is also referenced via
// `extern bool g_headlessMode;` from SparkEngineWindows.cpp / SparkEngineLinux.cpp
// (which write it) and ModuleManager.cpp (which reads it). Promoting it to
// std::atomic<bool> here without updating those extern declarations in lockstep
// would be an ODR type mismatch. In practice it is written once at startup
// (command-line parse) before worker threads spawn, so the read is effectively
// safe; a proper fix (atomic + coordinated extern-decl update, or folding the
// flag into EngineRuntime state) must touch those out-of-lane TUs together.
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
            if (Spark::ContainerUtils::Contains(entryMap, dep))
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
    enum class PreparationFailure
    {
        None,
        Cycle,
        Exception
    };

    PreparationFailure preparationFailure = PreparationFailure::None;
    std::vector<SubsystemEntry*> sorted;
    {
        std::unique_lock<std::recursive_mutex> lifecycleLock(m_lifecycleMutex, std::try_to_lock);
        if (!lifecycleLock.owns_lock())
        {
            std::fputs("[EngineContext] InitializeAll rejected a concurrent lifecycle transition\n", stderr);
            return false;
        }

        if (m_lifecycleState.load(std::memory_order_acquire) == LifecycleState::Initialized)
            return true;
        if (m_lifecycleState.load(std::memory_order_acquire) != LifecycleState::Idle)
        {
            std::fputs("[EngineContext] InitializeAll rejected a reentrant or failed lifecycle transition\n", stderr);
            return false;
        }

        // Claim the transition and snapshot the graph while registration is
        // excluded, then release the mutex before invoking logger/user code.
        m_lifecycleState.store(LifecycleState::Initializing, std::memory_order_release);
        try
        {
            if (!TopologicalSort(sorted))
            {
                preparationFailure = PreparationFailure::Cycle;
            }
            else
            {
                m_initOrder.clear();
                m_initOrder.reserve(sorted.size());
            }
        }
        catch (...)
        {
            preparationFailure = PreparationFailure::Exception;
        }
    }

    if (preparationFailure != PreparationFailure::None)
    {
        BestEffortLifecycleDiagnostic(
            [preparationFailure]
            {
                if (preparationFailure == PreparationFailure::Cycle)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: dependency cycle detected in subsystem graph");
                }
                else
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: lifecycle graph preparation threw an exception");
                }
            });
        m_lifecycleState.store(LifecycleState::Idle, std::memory_order_release);
        return false;
    }

    BestEffortLifecycleDiagnostic(
        [&]
        {
            SPARK_TRACE_ENTER(Spark::LogCategory::Core);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "EngineContext::InitializeAll — %zu subsystems registered",
                           m_subsystemEntries.size());
        });

    for (auto* entry : sorted)
    {
        try
        {
            if (entry->initFn)
            {
                BestEffortLifecycleDiagnostic(
                    [&]
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "EngineContext: initializing subsystem (type=%p)",
                                        entry->type);
                    });
                if (!entry->initFn())
                {
                    const bool rollbackSucceeded = RollbackFailedInitializationNoexcept(*entry);
                    if (!rollbackSucceeded)
                        m_lifecycleState.store(LifecycleState::Failed, std::memory_order_release);
                    BestEffortLifecycleDiagnostic(
                        [&]
                        {
                            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                            "EngineContext: subsystem initialization failed (type=%p)", entry->type);
                        });
                    if (rollbackSucceeded)
                        m_lifecycleState.store(LifecycleState::Idle, std::memory_order_release);
                    return false;
                }
            }
        }
        catch (const std::exception& exception)
        {
            const bool rollbackSucceeded = RollbackFailedInitializationNoexcept(*entry);
            if (!rollbackSucceeded)
                m_lifecycleState.store(LifecycleState::Failed, std::memory_order_release);
            BestEffortLifecycleDiagnostic(
                [&]
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: subsystem initialization threw (type=%p): %s", entry->type,
                                    exception.what());
                });
            if (rollbackSucceeded)
                m_lifecycleState.store(LifecycleState::Idle, std::memory_order_release);
            return false;
        }
        catch (...)
        {
            const bool rollbackSucceeded = RollbackFailedInitializationNoexcept(*entry);
            if (!rollbackSucceeded)
                m_lifecycleState.store(LifecycleState::Failed, std::memory_order_release);
            BestEffortLifecycleDiagnostic(
                [&]
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: subsystem initialization threw an unknown exception (type=%p)",
                                    entry->type);
                });
            if (rollbackSucceeded)
                m_lifecycleState.store(LifecycleState::Idle, std::memory_order_release);
            return false;
        }

        entry->initialized = true;
        m_initOrder.push_back(entry->type);
    }

    BestEffortLifecycleDiagnostic(
        [&]
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "EngineContext::InitializeAll — all %zu subsystems initialized",
                           m_initOrder.size());
        });
    m_lifecycleState.store(LifecycleState::Initialized, std::memory_order_release);
    return true;
}

bool EngineContext::CleanupInitializedSubsystemsNoexcept() noexcept
{
    bool cleanupSucceeded = true;
    for (auto it = m_initOrder.rbegin(); it != m_initOrder.rend(); ++it)
    {
        auto entryIt = std::find_if(m_subsystemEntries.begin(), m_subsystemEntries.end(),
                                    [&](const SubsystemEntry& e) { return e.type == *it; });
        if (entryIt == m_subsystemEntries.end() || !entryIt->initialized)
            continue;

        // Clear the flag before invoking user code so a reentrant attempt cannot
        // cause this resource to be shut down twice.
        entryIt->initialized = false;
        if (!entryIt->shutdownFn)
            continue;

        try
        {
            entryIt->shutdownFn();
        }
        catch (const std::exception& exception)
        {
            cleanupSucceeded = false;
            BestEffortLifecycleDiagnostic(
                [&]
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext: subsystem shutdown threw (type=%p): %s",
                                    entryIt->type, exception.what());
                });
        }
        catch (...)
        {
            cleanupSucceeded = false;
            BestEffortLifecycleDiagnostic(
                [&]
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: subsystem shutdown threw an unknown exception (type=%p)",
                                    entryIt->type);
                });
        }
    }

    m_initOrder.clear();
    return cleanupSucceeded;
}

bool EngineContext::RollbackFailedInitializationNoexcept(SubsystemEntry& failedEntry) noexcept
{
    bool rollbackSucceeded = true;
    if (failedEntry.shutdownFn)
    {
        try
        {
            failedEntry.shutdownFn();
        }
        catch (const std::exception& exception)
        {
            rollbackSucceeded = false;
            BestEffortLifecycleDiagnostic(
                [&]
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: failed subsystem rollback threw (type=%p): %s", failedEntry.type,
                                    exception.what());
                });
        }
        catch (...)
        {
            rollbackSucceeded = false;
            BestEffortLifecycleDiagnostic(
                [&]
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                    "EngineContext: failed subsystem rollback threw an unknown exception (type=%p)",
                                    failedEntry.type);
                });
        }
    }

    return CleanupInitializedSubsystemsNoexcept() && rollbackSucceeded;
}

void EngineContext::ShutdownAll()
{
    {
        std::unique_lock<std::recursive_mutex> lifecycleLock(m_lifecycleMutex, std::try_to_lock);
        if (!lifecycleLock.owns_lock())
        {
            std::fputs("[EngineContext] ShutdownAll rejected a concurrent lifecycle transition\n", stderr);
            return;
        }

        const LifecycleState state = m_lifecycleState.load(std::memory_order_acquire);
        if (state == LifecycleState::Idle)
            return;
        if (state != LifecycleState::Initialized)
        {
            std::fputs("[EngineContext] ShutdownAll rejected a reentrant or failed lifecycle transition\n", stderr);
            return;
        }

        m_lifecycleState.store(LifecycleState::ShuttingDown, std::memory_order_release);
    }

    BestEffortLifecycleDiagnostic(
        [&]
        {
            SPARK_TRACE_ENTER(Spark::LogCategory::Core);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "EngineContext::ShutdownAll — shutting down %zu subsystems",
                           m_initOrder.size());
        });
    const bool cleanupSucceeded = CleanupInitializedSubsystemsNoexcept();
    m_lifecycleState.store(cleanupSucceeded ? LifecycleState::Idle : LifecycleState::Failed, std::memory_order_release);
}
