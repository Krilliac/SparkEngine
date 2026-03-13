/**
 * @file EngineBootstrap.cpp
 * @brief Dependency-ordered subsystem initialization and shutdown implementation
 */

#include "EngineBootstrap.h"

#include "../Utils/LogMacros.h"
#include "../Utils/Logger.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace Spark
{

    // ========================================================================
    // Lifecycle
    // ========================================================================

    EngineBootstrap::~EngineBootstrap()
    {
        if (m_initialized && !m_shutDown)
        {
            Shutdown();
        }
    }

    // ========================================================================
    // Registration
    // ========================================================================

    bool EngineBootstrap::Register(SubsystemDescriptor descriptor)
    {
        SPARK_TRACE_ENTER(LogCategory::Core);

        if (m_initialized)
        {
            SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap::Register called after Initialize() — ignoring '%s'",
                            descriptor.Name.c_str());
            return false;
        }

        if (descriptor.Name.empty())
        {
            SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap::Register called with empty subsystem name");
            return false;
        }

        // Reject duplicates
        if (FindEntry(descriptor.Name) != nullptr)
        {
            SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap::Register — subsystem '%s' is already registered",
                            descriptor.Name.c_str());
            return false;
        }

        m_entries.push_back(SubsystemEntry{.Descriptor = std::move(descriptor), .State = SubsystemState::Registered});
        return true;
    }

    size_t EngineBootstrap::RegisterAll(std::vector<SubsystemDescriptor> descriptors)
    {
        SPARK_TRACE_ENTER(LogCategory::Core);

        size_t count = 0;
        for (auto& desc : descriptors)
        {
            if (Register(std::move(desc)))
            {
                ++count;
            }
        }
        return count;
    }

    // ========================================================================
    // Initialize
    // ========================================================================

    bool EngineBootstrap::Initialize()
    {
        SPARK_TRACE_ENTER(LogCategory::Core);

        if (m_initialized)
        {
            SPARK_LOG_WARN(LogCategory::Core, "EngineBootstrap::Initialize called more than once — ignoring");
            return true;
        }

        SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: beginning initialization of %zu subsystems",
                       m_entries.size());

        // 1. Topological sort
        std::vector<std::string> sortedNames;
        if (!TopologicalSort(sortedNames))
        {
            SPARK_LOG_ERROR(LogCategory::Core,
                            "EngineBootstrap: topological sort failed (cycle detected). Aborting initialization");
            return false;
        }

        m_initOrder = sortedNames;
        m_initialized = true;
        bool allSucceeded = true;

        // 2. Walk sorted order, calling init functions
        for (const auto& name : sortedNames)
        {
            auto* entry = FindEntry(name);
            if (entry == nullptr)
            {
                // Should not happen after a successful sort, but guard anyway
                SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: internal error — '%s' not found after sort",
                                name.c_str());
                allSucceeded = false;
                continue;
            }

            // Check that all dependencies succeeded
            bool dependenciesMet = true;
            for (const auto& dep : entry->Descriptor.Dependencies)
            {
                const auto* depEntry = FindEntry(dep);
                if (depEntry == nullptr || depEntry->State != SubsystemState::Initialized)
                {
                    SPARK_LOG_ERROR(LogCategory::Core,
                                    "EngineBootstrap: skipping '%s' — dependency '%s' is not initialized", name.c_str(),
                                    dep.c_str());
                    dependenciesMet = false;
                    break;
                }
            }

            if (!dependenciesMet)
            {
                entry->State = SubsystemState::Failed;
                allSucceeded = false;
                continue;
            }

            // Call init function
            if (!entry->Descriptor.InitFunction)
            {
                // No init function means the subsystem is a sentinel/grouping node
                SPARK_LOG_DEBUG(LogCategory::Core, "EngineBootstrap: '%s' has no init function — marking initialized",
                                name.c_str());
                entry->State = SubsystemState::Initialized;
                m_initializedSubsystems.push_back(name);
                continue;
            }

            SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: initializing '%s'", name.c_str());

            bool success = false;
            try
            {
                success = entry->Descriptor.InitFunction();
            }
            catch (const std::exception& ex)
            {
                SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: '%s' init threw exception: %s", name.c_str(),
                                ex.what());
                success = false;
            }
            catch (...)
            {
                SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: '%s' init threw unknown exception", name.c_str());
                success = false;
            }

            if (success)
            {
                entry->State = SubsystemState::Initialized;
                m_initializedSubsystems.push_back(name);
                SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: '%s' initialized successfully", name.c_str());
            }
            else
            {
                entry->State = SubsystemState::Failed;
                allSucceeded = false;
                SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: '%s' initialization FAILED", name.c_str());
            }
        }

        if (allSucceeded)
        {
            SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: all %zu subsystems initialized successfully",
                           m_initializedSubsystems.size());
        }
        else
        {
            SPARK_LOG_WARN(LogCategory::Core,
                           "EngineBootstrap: initialization completed with failures (%zu/%zu succeeded)",
                           m_initializedSubsystems.size(), sortedNames.size());
        }

        return allSucceeded;
    }

    // ========================================================================
    // Shutdown
    // ========================================================================

    void EngineBootstrap::Shutdown()
    {
        SPARK_TRACE_ENTER(LogCategory::Core);

        if (m_shutDown)
        {
            return;
        }
        m_shutDown = true;

        SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: shutting down %zu subsystems",
                       m_initializedSubsystems.size());

        // Walk initialized subsystems in reverse order
        for (auto it = m_initializedSubsystems.rbegin(); it != m_initializedSubsystems.rend(); ++it)
        {
            auto* entry = FindEntry(*it);
            if (entry == nullptr)
            {
                continue;
            }

            if (!entry->Descriptor.ShutdownFunction)
            {
                SPARK_LOG_DEBUG(LogCategory::Core, "EngineBootstrap: '%s' has no shutdown function — skipping",
                                it->c_str());
                entry->State = SubsystemState::ShutDown;
                continue;
            }

            SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: shutting down '%s'", it->c_str());

            try
            {
                entry->Descriptor.ShutdownFunction();
            }
            catch (const std::exception& ex)
            {
                SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: '%s' shutdown threw exception: %s", it->c_str(),
                                ex.what());
            }
            catch (...)
            {
                SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: '%s' shutdown threw unknown exception",
                                it->c_str());
            }

            entry->State = SubsystemState::ShutDown;
        }

        m_initializedSubsystems.clear();
        SPARK_LOG_INFO(LogCategory::Core, "EngineBootstrap: shutdown complete");
    }

    // ========================================================================
    // Queries
    // ========================================================================

    bool EngineBootstrap::IsInitialized(std::string_view name) const
    {
        const auto* entry = FindEntry(name);
        return entry != nullptr && entry->State == SubsystemState::Initialized;
    }

    SubsystemState EngineBootstrap::GetState(std::string_view name) const
    {
        const auto* entry = FindEntry(name);
        if (entry == nullptr)
        {
            return SubsystemState::Registered; // default / not-found
        }
        return entry->State;
    }

    const std::vector<std::string>& EngineBootstrap::GetInitOrder() const
    {
        return m_initOrder;
    }

    size_t EngineBootstrap::GetSubsystemCount() const
    {
        return m_entries.size();
    }

    // ========================================================================
    // Topological Sort — Kahn's Algorithm
    // ========================================================================

    bool EngineBootstrap::TopologicalSort(std::vector<std::string>& sortedNames) const
    {
        sortedNames.clear();

        // Build name -> index mapping
        std::unordered_map<std::string, size_t> nameToIndex;
        nameToIndex.reserve(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            nameToIndex[m_entries[i].Descriptor.Name] = i;
        }

        const size_t n = m_entries.size();

        // Build adjacency list and in-degree counts
        // Edge: dependency -> dependent (dep must come before dependent)
        std::vector<std::vector<size_t>> adjacency(n);
        std::vector<size_t> inDegree(n, 0);

        for (size_t i = 0; i < n; ++i)
        {
            for (const auto& dep : m_entries[i].Descriptor.Dependencies)
            {
                auto depIt = nameToIndex.find(dep);
                if (depIt == nameToIndex.end())
                {
                    // Missing dependency — log a warning but treat as a soft error.
                    // The subsystem will be skipped during init because its dependency
                    // won't be in Initialized state.
                    SPARK_LOG_WARN(LogCategory::Core,
                                   "EngineBootstrap: subsystem '%s' depends on '%s' which is not registered",
                                   m_entries[i].Descriptor.Name.c_str(), dep.c_str());
                    continue;
                }

                // dep -> i  (dep must be initialized before i)
                adjacency[depIt->second].push_back(i);
                ++inDegree[i];
            }
        }

        // Seed queue with all nodes that have zero in-degree
        std::queue<size_t> ready;
        for (size_t i = 0; i < n; ++i)
        {
            if (inDegree[i] == 0)
            {
                ready.push(i);
            }
        }

        // Process
        sortedNames.reserve(n);
        while (!ready.empty())
        {
            size_t current = ready.front();
            ready.pop();
            sortedNames.push_back(m_entries[current].Descriptor.Name);

            for (size_t neighbor : adjacency[current])
            {
                --inDegree[neighbor];
                if (inDegree[neighbor] == 0)
                {
                    ready.push(neighbor);
                }
            }
        }

        if (sortedNames.size() != n)
        {
            // Cycle detected — report which subsystems are involved
            SPARK_LOG_ERROR(LogCategory::Core, "EngineBootstrap: dependency cycle detected among %zu subsystem(s):",
                            n - sortedNames.size());

            std::unordered_set<std::string> sorted(sortedNames.begin(), sortedNames.end());
            for (const auto& entry : m_entries)
            {
                if (!sorted.contains(entry.Descriptor.Name))
                {
                    SPARK_LOG_ERROR(LogCategory::Core, "  - '%s'", entry.Descriptor.Name.c_str());
                }
            }
            return false;
        }

        return true;
    }

    // ========================================================================
    // FindEntry helpers
    // ========================================================================

    EngineBootstrap::SubsystemEntry* EngineBootstrap::FindEntry(std::string_view name)
    {
        auto it =
            std::ranges::find_if(m_entries, [name](const SubsystemEntry& e) { return e.Descriptor.Name == name; });
        return it != m_entries.end() ? &(*it) : nullptr;
    }

    const EngineBootstrap::SubsystemEntry* EngineBootstrap::FindEntry(std::string_view name) const
    {
        auto it =
            std::ranges::find_if(m_entries, [name](const SubsystemEntry& e) { return e.Descriptor.Name == name; });
        return it != m_entries.end() ? &(*it) : nullptr;
    }

} // namespace Spark
