/**
 * @file EntityArchetype.cpp
 * @brief Entity archetype system implementation
 */

#include "EntityArchetype.h"
#include "../../Utils/Validate.h"

#include <algorithm>

namespace Spark::ECS
{

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void EntityArchetypeSystem::Initialize()
    {
        m_archetypes.clear();
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "EntityArchetypeSystem initialized");
    }

    void EntityArchetypeSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "EntityArchetypeSystem shutting down (%zu archetypes)",
                       m_archetypes.size());
        m_archetypes.clear();
        m_initialized = false;
    }

    // =========================================================================
    // Registration
    // =========================================================================

    void EntityArchetypeSystem::RegisterArchetype(const Archetype& archetype)
    {
        if (!m_initialized || archetype.name.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "EntityArchetypeSystem::RegisterArchetype failed — %s",
                           !m_initialized ? "not initialized" : "empty name");
            return;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Registered archetype '%s' (category: '%s')", archetype.name.c_str(),
                       archetype.category.c_str());
        m_archetypes[archetype.name] = archetype;
    }

    void EntityArchetypeSystem::RemoveArchetype(const std::string& name)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Removing archetype '%s'", name.c_str());
        m_archetypes.erase(name);
    }

    // =========================================================================
    // Queries
    // =========================================================================

    const Archetype* EntityArchetypeSystem::GetArchetype(const std::string& name) const
    {
        auto it = m_archetypes.find(name);
        return (it != m_archetypes.end()) ? &it->second : nullptr;
    }

    std::vector<const Archetype*> EntityArchetypeSystem::GetArchetypesByCategory(const std::string& category) const
    {
        std::vector<const Archetype*> result;

        for (const auto& [name, archetype] : m_archetypes)
        {
            if (archetype.category == category)
            {
                result.push_back(&archetype);
            }
        }

        return result;
    }

    std::vector<std::string> EntityArchetypeSystem::GetAllArchetypeNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_archetypes.size());

        for (const auto& [name, archetype] : m_archetypes)
        {
            names.push_back(name);
        }

        return names;
    }

    bool EntityArchetypeSystem::HasArchetype(const std::string& name) const
    {
        return m_archetypes.contains(name);
    }

} // namespace Spark::ECS
